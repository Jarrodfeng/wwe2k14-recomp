// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// lane 194 (Ring D3, docs/renderer/SKIP_GAME_WORK_MAP.md 16.3 R0 / 16.7): the ring's per-call node rows. One RingRows per ring model call (a call of sub_82C04DF0 whose model address is in the
// table at [0x836F4C74]); `first_node` / `n_nodes` index kScRingNodes. Unlike kScArenaWorlds the entry is NOT deduplicated by (model, r5): a rope side's three heights share one r5 buffer that the
// game rewrites between heights, so the rows are copied AT THE CALL and every call has its own entry. Stamped with the hook's guest frame (a consumer joins by that stamp, never by the record index).
// The join key is (frame, object, call) with `call` = the index among ALL the object's model calls of the frame (= the LightRef ordinal of that call); `keyed` = the index among the object's
// RECORDED entries of the frame (the sequence ordinal a binder will use). The game's palette is 64 bytes a node (4x4, translation in m[12..14]); the pool holds the rows of each node as the packer cuts them.
inline constexpr uint32_t kRingRowsLayout = 1;
inline constexpr uint32_t kRingMaxNodes = 96;      // a call with more nodes is truncated (kRrCapped, counted); the ring's biggest is the skirt at 66
enum RingClass : uint8_t { kRcNone = 0, kRcRopeHi, kRcRopeLo, kRcProxy, kRcCoverHi, kRcCoverLo, kRcTurnbuckle, kRcPostHi, kRcPostLo, kRcCanvas, kRcSkirt, kRcCount };
enum RingRowsFlags : uint32_t {
  kRrForeignR5 = 1,       // r5 was not [model+200] (the rope context buffer, sub_82C13EC8's caller array): still what the packer reads
  kRrCountForwarded = 2,  // the node count came through [this+220] (sub_82BD3AB8's forward; vtable slot 2 verified to be the same function)
  kRrCountUnknown = 4,    // the node count could not be read: n_nodes = 0, no rows
  kRrCapped = 8,          // count above kRingMaxNodes: only the first kRingMaxNodes rows are kept
  kRrUnreadable = 16,     // the rows at r5 were not (fully) readable: n_nodes = 0, no rows
  kRrAlphaUnread = 32,    // [model+252] (the f1 the vtable slot 13 passes) was not readable; alpha = 0
  kRrRecomputed = 64,     // S103 C3: not a copy of a call: the rope rows our recompute made from the rope grid (src\scene_live\ring_recompute_core.h); call = 3 * side + rope, class = rope_hi stands in (LOD not replicated)
};
struct RingRows {
  uint8_t kind = 1;              // 1 = a call's rows (the only kind in layout 1)
  uint8_t layout = uint8_t(kRingRowsLayout);
  uint8_t klass = 0;             // RingClass of the model address
  uint8_t pad = 0;
  uint32_t object = 0;           // the model address (DrawOrigin::object)
  uint32_t frame = 0;            // the hook's guest frame (low 32 bits) when the call ran
  uint32_t call = 0;             // index among ALL the object's model calls of the frame (the LightRef ordinal)
  uint32_t keyed = 0;            // index among this object's recorded RingRows of the frame
  uint32_t r5 = 0;               // the palette pointer the call carried (guest address; the rope buffer's identity)
  uint32_t first_node = 0;       // into kScRingNodes
  uint16_t n_nodes = 0;
  uint16_t pad2 = 0;
  uint32_t flags = 0;            // RingRowsFlags
  float alpha = 0.f;             // [model+252]: the MdlAlpha c185 source [L] (SKIP_GAME_WORK_MAP.md 16.1: the proof compare decides)
};
static_assert(sizeof(RingRows) == 40, "RingRows is a flat POD table");
struct RingNodeRows {
  float rows[12];                // VS c76..c78 of the node: row k = (m[k], m[4+k], m[8+k], m[12+k]) of the node's 4x4
};
static_assert(sizeof(RingNodeRows) == 48, "RingNodeRows is a flat POD table");

}  // namespace rdr_scene
