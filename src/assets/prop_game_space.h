// Props of other pacs, from the files, in the GAME's space (S101 lane 84 = PO; docs\renderer\UNCLAIMED_MODEL_PARTS.md section 3.2; as built: docs\renderer\PROPS_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own pac\evt\evtobj.pac, pac\bg\bgEtc.pac and pac\m2.pac at run time; nothing is kept on disk.
//
// Three objects the arena, wrestler and ring hooks pass on as NotOurs (lane 79's census, Check B / Check F frame 200, pass site 0x82C0A950):
//   table  0xA4FB27F0  the commentary table: evtobj.pac PACHs whose model has 7 meshes of (284, 92, 652, 38, 385, 99, 29) vertices (17 PACHs, identical geometry, differing top texture)
//   mic    0xA4FD4CB0  the ring announcer's microphone: evtobj.pac PACHs whose model has 4 meshes of (10, 24, 171, 272) vertices (4, 135, 301; the counts alone do not name it)
//   steps  0xAA141900  the two ring steps: bgEtc.pac PACH 9 keys 29 / 32 / 35 and PACH 62 key 32, m2.pac PACH 0 and 2 key 1000 keys 29 / 32 / 35 (10 leaves, identical positions)
// This file is the FILE side only (the same mesh records as stored, the textures their materials name); the vertex / index writer is the ring's (ring_game_space.h WriteVertices /
// BuildIndices: the same skinned prop vertex, prop_host_vertex.h), so a RingMesh here has `entry` = the source id below.
// Threads: Load on a worker (it takes the loader's process-wide mutex and opens the three pacs itself, leaving the loader's one-slot cache alone); everything else is plain data.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ring_game_space.h"

namespace assets::prop_gs {

enum class Group : uint8_t { Table, Mic, Steps, Scene };   // lane 137: Scene = bgEtc.pac props placed by an arena or a show (WM30: the steel cage, the commentary desk, its monitors, one more prop; WHATS_LEFT N9)
inline const char* GroupName(Group g) { return g == Group::Table ? "table" : g == Group::Mic ? "mic" : g == Group::Steps ? "steps" : "scene"; }   // lane 137: + scene

struct Source {
  int id = -1;            // the mesh `entry` of ring_gs::RingMesh (index in PropSet::sources)
  Group group = Group::Table;
  std::string label;      // "evtobj PACH 305", "bgEtc p9/32", "m2 p0/1000/32"
  bool family32 = false;  // steps only: the "32" family (the leaves whose textures are the stream's; the unit test names the expected joins by it)
};

struct PropSet {
  std::vector<Source> sources;
  std::vector<ring_gs::RingMesh> meshes;
  std::vector<Texture> textures;
  size_t Bytes() const {
    size_t n = 0;
    std::vector<const void*> seen;   // review F7: the meshes of one model share its blob: count each once
    for (const ring_gs::RingMesh& m : meshes) {
      if (!m.blob || std::find(seen.begin(), seen.end(), m.blob.get()) != seen.end()) continue;
      seen.push_back(m.blob.get());
      n += m.blob->size();
    }
    for (const Texture& t : textures) n += t.data.size();
    return n;
  }
  const ring_gs::RingMesh* Find(int source, int part) const {
    for (const ring_gs::RingMesh& m : meshes)
      if (m.entry == source && m.part == part) return &m;
    return nullptr;
  }
  // Test only (review F3): the sources in the opposite order (ids renumbered), so a join that only works because the pac lists the right member first is caught: the tie-break by texture content must still pick the same members.
  void Reverse() {
    const int n = int(sources.size());
    std::reverse(sources.begin(), sources.end());
    for (int i = 0; i < n; ++i) sources[size_t(i)].id = i;
    for (ring_gs::RingMesh& m : meshes) m.entry = n - 1 - m.entry;
  }
  const Source* SourceOf(int id) const { return id >= 0 && size_t(id) < sources.size() ? &sources[size_t(id)] : nullptr; }
};

bool Init(const std::string& game_path);
// Loads every source that exists. False + err when a pac cannot be read or no source was found.
bool Load(PropSet& out, std::string& err);

}  // namespace assets::prop_gs
