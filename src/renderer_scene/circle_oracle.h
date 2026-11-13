// The circle blobs' capture-side oracle: a capture's circle draws -> the guest memory the game's upload would have read -> the SHIPPED writer's tracker -> the record entries
// (Session 101, lane 136; docs/renderer/SHADOW_CIRCLES.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game, SDK or renderer includes: fview's circles
// check and tools/shadow/circles_unit.cpp both run THIS code, so the record the hook plays in the picture proof is the record the writer's tracker produces from guest bytes.
//
// The input file comes from tools/shadow/circles_oracle.py (the stream's circle constants and the disc mesh as the capture holds them; numbers only, under out\, never committed).
// What is faked: the guest. Constants are laid out big-endian where sub_82812E90 holds them (r1+256 ViewProj, r1+80 ShadowParam, r1+320 the 21 points), the 9-word slot table, and the
// circle object's mesh pointers; the tracker is then called exactly as the op-21 hook calls it (return address, r4, r5, r6, r29, r30). What is NOT faked is the tracker, the mesh
// conversion, and BuildRows.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <map>
#include <string>
#include <vector>

#include "../scene_live/circle_hook.h"
#include "record_io.h"

namespace rdr_scene::circles::oracle {

inline constexpr uint32_t kMagic = 0x43524943u, kVersion = 1;

struct Frame {
  uint32_t capture_frame = 0, guest_frame = 0;
  std::vector<std::array<float, kRows * 4>> draws;   // c0..c28 per circle draw, in draw order
};
struct File {
  std::vector<uint8_t> vertices;    // the stream's host layout, vertex_count * 32
  std::vector<uint16_t> indices;
  std::vector<Frame> frames;
};

inline bool Load(const std::string& path, File& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return error = "cannot open " + path, false;
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto need = [&](size_t at, size_t n) { return at + n <= b.size(); };
  uint32_t head[4];
  if (!need(0, 16)) return error = path + ": truncated header", false;
  std::memcpy(head, b.data(), 16);
  if (head[0] != kMagic || head[1] != kVersion) return error = path + ": not a circles oracle file (magic / version)", false;
  const uint32_t nv = head[2], ni = head[3];
  size_t at = 16;
  if (nv == 0 || nv > kCircleMeshVertices || ni == 0 || ni > kCircleMeshIndices || !need(at, size_t(nv) * 32 + size_t(ni) * 2 + 4)) return error = path + ": the mesh does not fit", false;
  out.vertices.assign(b.begin() + at, b.begin() + at + size_t(nv) * 32);
  at += size_t(nv) * 32;
  out.indices.resize(ni);
  std::memcpy(out.indices.data(), b.data() + at, size_t(ni) * 2);
  at += size_t(ni) * 2;
  uint32_t nf = 0;
  std::memcpy(&nf, b.data() + at, 4);
  at += 4;
  out.frames.clear();
  out.frames.reserve(nf);
  for (uint32_t i = 0; i < nf; ++i) {
    if (!need(at, 12)) return error = path + ": truncated frame table", false;
    Frame f;
    uint32_t n = 0;
    std::memcpy(&f.capture_frame, b.data() + at, 4);
    std::memcpy(&f.guest_frame, b.data() + at + 4, 4);
    std::memcpy(&n, b.data() + at + 8, 4);
    at += 12;
    if (n > 16 || !need(at, size_t(n) * sizeof(float) * kRows * 4)) return error = path + ": truncated frame " + std::to_string(i), false;
    f.draws.resize(n);
    for (uint32_t d = 0; d < n; ++d) {
      std::memcpy(f.draws[d].data(), b.data() + at, sizeof(float) * kRows * 4);
      at += sizeof(float) * kRows * 4;
    }
    out.frames.push_back(std::move(f));
  }
  return true;
}

// ---- the fake guest ---------------------------------------------------------------------------------------------------------------------------------------------------
class FakeGuest {
 public:
  void Put(uint32_t addr, const void* data, size_t n) {
    std::vector<uint8_t>& r = regions_[addr];
    r.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
  }
  void PutWord(uint32_t addr, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    Put(addr, b, 4);
  }
  void PutFloats(uint32_t addr, const float* f, size_t count) {   // big-endian words
    std::vector<uint8_t> b(count * 4);
    for (size_t i = 0; i < count; ++i) {
      uint32_t v;
      std::memcpy(&v, &f[i], 4);
      b[4 * i] = uint8_t(v >> 24), b[4 * i + 1] = uint8_t(v >> 16), b[4 * i + 2] = uint8_t(v >> 8), b[4 * i + 3] = uint8_t(v);
    }
    Put(addr, b.data(), b.size());
  }
  bool Read(uint32_t addr, void* dst, size_t n) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return false;
    --it;
    if (addr < it->first || uint64_t(addr) + n > uint64_t(it->first) + it->second.size()) return false;
    std::memcpy(dst, it->second.data() + (addr - it->first), n);
    return true;
  }
  void Clear() { regions_.clear(); }

 private:
  std::map<uint32_t, std::vector<uint8_t>> regions_;
};

inline constexpr uint32_t kObject = 0x70000000u, kVertexBuf = 0x70010000u, kIndexBuf = 0x70020000u, kStackBase = 0x7F000000u, kCharBase = 0x40000000u;

// A D3D resource object's header as the game holds it: 64 bytes of big-endian words.
inline void PutObject(FakeGuest& g, uint32_t addr, std::initializer_list<std::pair<uint32_t, uint32_t>> words) {
  uint8_t b[64] = {};
  for (const auto& w : words) {
    b[w.first] = uint8_t(w.second >> 24), b[w.first + 1] = uint8_t(w.second >> 16), b[w.first + 2] = uint8_t(w.second >> 8), b[w.first + 3] = uint8_t(w.second);
  }
  g.Put(addr, b, sizeof b);
}
inline constexpr uint32_t kVertexObject = 0x70008000u, kIndexObject = 0x70009000u;

// Lays the mesh and the slot table out once (what sub_82812E90 finds in guest memory). F1: [circle+36] / [circle+40] are RESOURCE OBJECTS: the vertex buffer object carries the fetch word (address,
// type in the low two bits) at +24 and the size at +28; the index buffer object the data address at +24 and its flags at +0 (top bit = 32-bit). The low bits are set here on purpose (the reader masks them).
inline void SetupMesh(FakeGuest& g, const File& f, uint32_t index_flags = 0x00000102u) {
  CircleMesh m;
  std::memcpy(m.vertices, f.vertices.data(), f.vertices.size());
  m.vertex_count = uint32_t(f.vertices.size() / 32), m.index_count = uint32_t(f.indices.size());
  std::memcpy(m.indices, f.indices.data(), f.indices.size() * 2);
  std::vector<uint8_t> vb, ib;
  MeshToGuest(m, vb, ib);
  g.Put(kVertexBuf, vb.data(), vb.size());
  g.Put(kIndexBuf, ib.data(), ib.size());
  PutObject(g, kVertexObject, {{0, 0x00000001u}, {24, kVertexBuf | 3u}, {28, uint32_t(vb.size())}});
  PutObject(g, kIndexObject, {{0, index_flags}, {24, kIndexBuf}});
  PutObject(g, kObject, {{live::circles::kMeshVertexPtr, kVertexObject}, {live::circles::kMeshIndexPtr, kIndexObject}, {live::circles::kMeshIndexCount, m.index_count}});
  for (uint32_t s = 0; s < live::circles::kSlots; ++s) g.PutWord(live::circles::kSlotTable + 4 * s, kCharBase + 0x100 * s);
}

// One frame: the draws as ONE call of sub_82812E90 makes them (ViewProj once, then ParamReturn + PointsReturn per character), at hook frame `stamp`.
inline void DriveFrame(live::circles::Tracker& t, FakeGuest& g, const Frame& frame, uint32_t stamp) {
  const live::circles::ReadBytes rd = [&g](uint32_t a, void* dst, size_t n) { return g.Read(a, dst, n); };
  for (size_t i = 0; i < frame.draws.size(); ++i) {
    const std::array<float, kRows * 4>& rows = frame.draws[i];
    const uint32_t stack = kStackBase + 0x1000u * uint32_t(i);   // this character's r1 (the arrays sit at the same offsets from it as in sub_82812E90's frame)
    g.PutFloats(stack + 256, rows.data() + 16, 16);              // c4-c7
    g.PutFloats(stack + 80, rows.data(), 4);                     // c0
    g.PutFloats(stack + 320, rows.data() + 32, 4 * kCirclePoints);   // c8-c28
    const uint32_t slot_addr = live::circles::kSlotTable + 4 * uint32_t(i);
    if (i == 0) t.OnConstants(live::circles::kViewProjReturn, 4, stack + 256, 4, kObject, slot_addr, stamp, rd);
    t.OnConstants(live::circles::kParamReturn, 0, stack + 80, 1, kObject, slot_addr, stamp, rd);
    t.OnConstants(live::circles::kPointsReturn, 8, stack + 320, kCirclePoints, kObject, slot_addr, stamp, rd);
  }
}

// A scene record holding the frame's entries (and the mesh), as the contributor writes it.
inline void FillRecord(SceneTables& t, const std::vector<CircleShadow>& entries, const CircleMesh* mesh, uint64_t guest_frame) {
  t.header.guest_frame = guest_frame;
  t.circle_shadows = entries;
  if (mesh) t.circle_mesh.assign(1, *mesh);
}

}  // namespace rdr_scene::circles::oracle
