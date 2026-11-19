// The ring ropes' shadow proxy in the plane mask (Session 103, lane B2F; docs\renderer\MAT_SHADOW.md, SHADOW_PROMOTION_MAP.md 1.2, shadows.md 6.2, ring.md section 4).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. The record side (RopeInput) and the proxy's state (RopeProxyState) of live_map_rope.cpp; included by live_maps_e2.h.
//
// WHAT THE GAME DOES [C: stream, run 29 / z7147 frames, 24 of the 48 mask draws]: the ring's rope plane-shadow proxy (pac entry 956: two meshes, one skin pair a vertex, a 20-slot palette each) is drawn
// 12 times a frame (4 sides x 3 ropes) into the 512 x 512 plane mask. The palette of an instance is the rope's own node rows (the rope context buffer, ring_recompute_core.h kBufOff), slot k = pool row
// palette[k] of the mesh record (NOT palette[k] - 1: measured on every slot of both parts); the eye point of the instance is the ring kind (4..7) whose side the rope is on (the rope at +z is lit from (0, -800, +1000): the SAME sign, measured on all 24 draws).
// WHAT WE DO: the same draws through our mask program (live_map_plane.cpp RecordPlane), the rows from the record's ring rows (kScRingRows / kScRingNodes: ours with kRrRecomputed, else the game's), the
// eyes from the record's PlaneEyes kinds 4..7, the mesh from the player's own bgNN.pac (assets::ring_gs, one worker load per arena). A rope's eye is picked by DIRECTION (the largest dot product of the
// eye's x z with the rope's centre), so no side -> kind table is assumed.
// NOT IN THE RECORD [U]: the game's draw condition ([X + 96] != 0 and sub_82BA77A8() != 1): a rope row set exists in the record only when the ring's callback ran its rope fill (which tests [X + 96]), so a
// recorded rope is a drawn one; the sub_82BA77A8 test is not read.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../renderer/renderer.h"

namespace assets::ring_gs { struct RingArena; }

namespace rdr_scene {

struct SceneTables;

inline constexpr uint32_t kRopeMax = 12;   // 4 sides x 3 ropes

// The rope node rows of one record: up to 12 ropes, 12 floats a node (VS c76..c78 rows of the node, translation at 3 / 7 / 11).
struct RopeInput {
  int arena = -1;                // the arena of the record's pac\bg\bgNN.pac (-1: none named)
  uint32_t count = 0;            // ropes found
  uint32_t calls_seen = 0;       // rope ring-row entries of the record (any class, any provenance)
  uint32_t unusable = 0;         // rope entries with no nodes / an unreadable range
  bool recomputed = false;       // the rows are ours (kRrRecomputed) rather than the game's own copy
  struct Rope {
    uint32_t first = 0, n = 0;   // node rows `rows` [first * 12, (first + n) * 12)
    float cx = 0.f, cz = 0.f;    // the mean translation x / z of the nodes (the rope's side)
  } rope[kRopeMax];
  std::vector<float> rows;
};
void BuildRopeInput(const SceneTables& t, RopeInput& out);

// The proxy's state in one hook: the file mesh (a worker loads it), its GPU buffers, the per-frame notes.
struct RopeProxyState {
  int arena = -1;                          // the arena whose proxy is loaded / loading
  std::atomic<int> load_state{0};          // 0 none, 1 loading, 2 done (loaded / error valid)
  std::shared_ptr<assets::ring_gs::RingArena> loaded;
  std::string error;
  std::thread worker;
  bool uploaded = false;                   // GPU buffers made from `loaded`
  bool failed = false;                     // the load or the mesh rules failed: kept (never retried every frame)
  struct Part {
    uint32_t vb = 0, sv = 0, ib = 0, index_count = 0;   // byte offsets into the three buffers
    uint32_t palette_count = 0;
    uint32_t palette[20] = {};             // the record's palette numbers: slot k -> pool row palette[k]
  };
  std::vector<Part> parts;
  rdr::BufferId vb = rdr::kNoBuffer, sv = rdr::kNoBuffer, ib = rdr::kNoBuffer;
  uint16_t layout = 0;
  bool layout_ready = false;
  // per frame
  uint32_t draws = 0;                      // proxy draws this frame
  uint32_t ropes_drawn = 0, ropes_dropped = 0;
  const char* note = "";                   // why nothing was drawn this frame ("" = drawn or no ropes in the record)
  ~RopeProxyState() {
    if (worker.joinable()) worker.join();
  }
};

}  // namespace rdr_scene
