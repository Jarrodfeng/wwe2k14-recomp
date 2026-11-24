// The ring in motion posing: the pieces' palettes while bit 0 of the byte at 0x837632FA is set, and the canvas (S110 lane RINGBREAK; docs\research\s110\RINGBREAK.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHY. When the ring breaks the game switches every ring piece to motion posing: a 45-slot ring rig ('RIDS', sub_82594F70) holds one motion per slot and sub_82595CB0(rids, slot) poses the slot's model from it
// (sub_82413120: the motion's pose into the model's root, then the world and skin vcalls into [model+200]; it does not advance time, see below) just before that piece's draw. The posts, turnbuckles, covers and
// ropes share ONE model per class, re-posed per instance, so a palette exists only right after its own pose call (not at the latch). Our build skips the ring colour callback (sub_825D0438) whole, so the pose never
// ran; the recompute counted the case and emitted no rows (the pieces drew at rest). The fix: in the skip branch, run the game's own sub_82595CB0 per slot and copy the model's [model+200] at once; the contributor
// turns the copies into the ordinary ring rows (kRrRecomputed) at the latch.
//
// SLOTS [C RINGBREAK.md section 1]: sub_82596040(rids, k) returns the slot's model (0 above 44; the switch over the ring table G = [0x836F4C74]); posts 5 + i, turnbuckles 9.., covers 21 + 3 corner + layer, ropes
// 33 + 3 side + height. The model is the one the slot switch returns: posts G+48, turnbuckle G+44, cover G+36, rope G+20 (the HIGH model of each class; the low LOD models are not in the switch, so the break mode has no
// low rows). The call index of a row = the index within its class in slot order.
// NO TIME STEP [C, read for this lane]: sub_82413120 -> sub_823DB1C0(obj, 1, 0) reads the motion's state ([obj+108] rotation / scale / translation words, [obj+156] count, [obj+116] flags) and writes only the model's node
// matrices ([[obj+32]+180] + 64 i, through sub_82C407E0 = quaternion to matrix and sub_82296630); it never writes the motion object. The time step is the RIDS update's own vcall +48 (sub_82598680), which keeps running.
//
// The pure part (the store, AppendRidsRows, AppendCanvas) has no game include (the offline unit runs it). The glue (PoseRidsRing) exists only where the SDK header is in (REX_EXTERN defined: scene_skip_nodes.h).
#pragma once
#if __has_include(<rex/hook.h>)
#include <rex/hook.h>   // REX_EXTERN: turns the glue below on wherever the SDK is (the offline unit has none)
#endif
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "../renderer_game/scene_frame.h"
#include "../renderer_scene/rows_ring.h"
#include "arena_world.h"   // RowsFromMatrix
#include "ring_recompute_core.h"
#include "ring_rows_core.h"

namespace rdr_scene::live::ringrids {

using ringrecompute::ModelPal;
using ringrecompute::RingCensus;

inline constexpr uint32_t kRidsByteAddr = 0x837632FA;   // bit 0 = motion posing (0x825CE338-0x825CE358, 0x825CFAD8)
inline constexpr uint32_t kFirstSlot = 5, kLastSlot = 44;   // the skipped callback's slots (posts 5..8, turnbuckles 9..20, covers 21..32, ropes 33..44); sub_82596040 returns 0 above 44
inline constexpr uint32_t kRidsRoot = 0x10000;              // a pointer below this is not a model
inline constexpr uint32_t kCanvasSlot = 56;                 // [G + 56] = the canvas model (sub_82608CB0; ring_rows_core.h kClassTable {56, kRcCanvas})
// The classes a RIDS slot can hold (bit in Inputs::rids and in the mask AppendRidsRows takes).
inline constexpr uint32_t kBitRope = 1, kBitTb = 2, kBitCover = 4, kBitPost = 8;
// The node counts a class is known to have: a count of 0 or above this is refused (the model's vtable gave a count that is not the piece's).
inline constexpr uint32_t kMaxNodesRope = 128, kMaxNodesTb = 8, kMaxNodesCover = 96, kMaxNodesPost = 96;

struct Slot {
  uint32_t k = 0, model = 0, palette = 0, n = 0;   // n = 0: the model could not be read (refused, counted)
  std::vector<float> pal;                          // n * 16 floats, host order, copied right after the slot's pose
};
struct Set {
  uint64_t frame = ~0ull;
  std::vector<Slot> slots;
  uint32_t refused = 0;   // refusals the glue made itself (a slot past 44 that still names a model)
};

// The glue's published copy, one set per hook frame (the ring callback's frame, the same clock NoteRing stamps).
class Store {
 public:
  void Publish(Set&& s) {
    std::lock_guard<std::mutex> lk(mu_);
    set_ = std::move(s);
  }
  bool Get(uint64_t frame, Set& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (set_.frame != frame) return false;
    out = set_;
    return true;
  }

 private:
  mutable std::mutex mu_;
  Set set_;
};
inline Store& The() {
  static Store s;
  return s;
}

struct Counts {
  uint32_t rope = 0, tb = 0, cover = 0, post = 0;
};
inline uint32_t BitOfClass(uint8_t cls) {
  switch (cls) {
    case kRcRopeHi: case kRcRopeLo: return kBitRope;
    case kRcTurnbuckle: return kBitTb;
    case kRcCoverHi: case kRcCoverLo: return kBitCover;
    case kRcPostHi: case kRcPostLo: return kBitPost;
    default: return 0;
  }
}
inline uint32_t MaxNodesOfBit(uint32_t bit) {
  return bit == kBitRope ? kMaxNodesRope : bit == kBitTb ? kMaxNodesTb : bit == kBitCover ? kMaxNodesCover : kMaxNodesPost;
}

// The rows of one hook frame's slot copies, for the classes whose own gates passed (`mask`). Class = the game's table (RingTable::ClassOf of the slot's model); call = the index within the class in slot order.
// A slot with a model below 0x10000, no class, a node count of 0 or above the class's, or a short copy gives no row and is counted `rids_refused`.
inline Counts AppendRidsRows(const Set& s, uint32_t stamp, uint32_t mask, const ringrows::RingTable& table, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  Counts out;
  cn.rids_refused += s.refused;
  uint32_t next[4] = {};   // the running call index per class (rope, turnbuckle, cover, post), in slot order
  for (const Slot& sl : s.slots) {
    if (sl.k < kFirstSlot || sl.k > kLastSlot || sl.model < kRidsRoot) { ++cn.rids_refused; continue; }
    const uint32_t bit = BitOfClass(table.ClassOf(sl.model));
    if (!bit) { ++cn.rids_refused; continue; }
    const uint32_t ci = bit == kBitRope ? 0u : bit == kBitTb ? 1u : bit == kBitCover ? 2u : 3u;
    const uint32_t call = next[ci]++;
    if (!(mask & bit)) continue;   // that class's own gates were closed this frame (counted there)
    if (sl.n == 0 || sl.n > MaxNodesOfBit(bit) || sl.pal.size() < size_t(sl.n) * 16) { ++cn.rids_refused; continue; }
    RingRows e;
    e.klass = bit == kBitRope ? kRcRopeHi : bit == kBitTb ? kRcTurnbuckle : bit == kBitCover ? kRcCoverHi : kRcPostHi;
    e.object = sl.model;
    e.frame = stamp;
    e.call = e.keyed = call;
    e.r5 = sl.palette;
    e.first_node = uint32_t(pool.size());
    e.n_nodes = uint16_t(std::min<uint32_t>(sl.n, kRingMaxNodes));
    e.flags = kRrRecomputed | (sl.n > kRingMaxNodes ? kRrCapped : 0u);
    e.alpha = 1.f;
    for (uint32_t k = 0; k < e.n_nodes; ++k) {
      RingNodeRows nr;
      arenaworld::RowsFromMatrix(&sl.pal[size_t(k) * 16], nr.rows);
      pool.push_back(nr);
    }
    rows.push_back(e);
    ++cn.rids_rows;
    (ci == 0 ? out.rope : ci == 1 ? out.tb : ci == 2 ? out.cover : out.post) += 1;
  }
  return out;
}

// The canvas (the mat): [G+56]'s palette as it stands at the latch, every frame (the game poses it in every match: the bounce, sub_82608CB0 +0x1F0..+0x214; and in the break by the motion). ONE row, call 0.
// `m` as ReadModelPal read it (m.model == 0: not there / not readable: no row, counted by the caller).
inline bool AppendCanvas(const ModelPal& m, uint32_t stamp, std::vector<RingRows>& rows, std::vector<RingNodeRows>& pool, RingCensus& cn) {
  if (!m.model || m.n == 0 || m.pal.size() < size_t(m.n) * 16) return false;
  RingRows e;
  e.klass = kRcCanvas;
  e.object = m.model;
  e.frame = stamp;
  e.call = e.keyed = 0;
  e.r5 = m.palette;
  e.first_node = uint32_t(pool.size());
  e.n_nodes = uint16_t(std::min<uint32_t>(m.n, kRingMaxNodes));
  e.flags = kRrRecomputed | (m.n > kRingMaxNodes ? kRrCapped : 0u);
  e.alpha = 1.f;
  for (uint32_t k = 0; k < e.n_nodes; ++k) {
    RingNodeRows nr;
    arenaworld::RowsFromMatrix(&m.pal[size_t(k) * 16], nr.rows);
    pool.push_back(nr);
  }
  rows.push_back(e);
  ++cn.canvas_rows;
  return true;
}

}  // namespace rdr_scene::live::ringrids

#ifdef REX_EXTERN
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

#include "../guest_call.h"

REX_EXTERN(sub_82594F70);   // the RIDS component (no argument) -> rids
REX_EXTERN(sub_82596040);   // (rids, slot) -> the slot's model, 0 above 44
REX_EXTERN(sub_82595CB0);   // (rids, slot): poses the slot's model from its motion into [model+200]

namespace rdr_scene::live::ringrids {

// The skip branch of the ring colour callback hook: with bit 0 set and the RIDS component there, pose each slot with the game's own call and copy its palette at once. Does nothing otherwise (the common case: one byte read).
// Runs on the hook's thread inside the skipped callback's place, so the thread is the one the game would have posed on. `hook_frame` = the frame NoteRing was given.
inline void PoseRidsRing(PPCContext& ctx, uint8_t* base, uint64_t hook_frame) {
  if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;   // mode 0 (every player): nothing is latched, nothing is posed
  const auto word = [&](uint32_t a, uint32_t& v) {
    if (a < kRidsRoot || a >= 0xC0000000u) return false;
    uint32_t raw = 0;
    std::memcpy(&raw, rex::memory::GuestPtr(base, a), 4);
    v = (raw >> 24) | ((raw >> 8) & 0xFF00u) | ((raw << 8) & 0xFF0000u) | (raw << 24);
    return true;
  };
  if ((*static_cast<const uint8_t*>(rex::memory::GuestPtr(base, kRidsByteAddr)) & 1u) == 0) return;
  wwe2k14::scene_director::Caller call(ctx, base);
  const uint32_t rids = call(sub_82594F70, {});
  if (rids < kRidsRoot) return;
  Set set;
  set.frame = hook_frame;
  for (uint32_t k = kFirstSlot; k <= kLastSlot; ++k) {
    const uint32_t model = call(sub_82596040, {rids, k});
    if (!model) break;   // the game's own bound: 0 ends the slots
    Slot sl;
    sl.k = k, sl.model = model;
    if (model >= kRidsRoot) {
      call(sub_82595CB0, {rids, k});   // the game's pose; the palette is valid only until the next instance of the class
      uint32_t pal = 0;
      int32_t n = 0;
      uint32_t nflags = 0;
      if (word(model + ringrows::kPaletteOffset, pal) && pal >= kRidsRoot && ringrows::NodeCount(word, model, n, nflags) && n > 0 && n <= int32_t(kMaxNodesRope)) {
        sl.palette = pal, sl.n = uint32_t(n);
        sl.pal.resize(size_t(n) * 16);
        for (uint32_t i = 0; i < uint32_t(n) * 16u && sl.n; ++i) {
          uint32_t w = 0;
          if (!word(pal + 4 * i, w)) { sl.n = 0, sl.pal.clear(); break; }
          std::memcpy(&sl.pal[i], &w, 4);
        }
      }
    }
    set.slots.push_back(std::move(sl));
  }
  if (call(sub_82596040, {rids, kLastSlot + 1}) != 0) ++set.refused;   // a slot past 44 that still names a model: not read, counted
  The().Publish(std::move(set));
}

}  // namespace rdr_scene::live::ringrids
#endif
