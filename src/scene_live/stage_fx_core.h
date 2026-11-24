// The stage's live writers, the pure rules (S103 lane C2; docs\renderer\STAGE_LIVE_WRITERS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; NO game, SDK or renderer-adapter includes: the contributor (stage_fx_contrib.h), the game glue (stage_fx_glue.h) and the offline unit
// (tools\scene\stage_fx_unit.cpp) run THESE functions. Marks: [C] read in a listing (cited), [L] a reading, [U] unknown.
//
// 1. Moving arena slots -> kFxSlotWorld rows.
//  * Where the slot's model object is [C listing generated\default\wwe2k14_recomp.16.cpp:4278 sub_824E5340 = CStage::MotionSetup(this = r27 = the stage object)]: the loop runs 399 iterations,
//    i = 0..398 (r26 = 399 counted down), with r31 = stage + 10952 + 4 i; `lwz r11,-4788(r31)` = the word at stage + 6164 + 4 i = the slot's MODEL OBJECT (0 = the slot has no model: the loop skips
//    it, `beq 0x824E54DC`); the motion id (u16, `lhz r10,0(r29)`) is at stage + 1224 + 12 i and the loop skips a slot whose id is 0; a motion player (208 bytes, tag 'OBJM') is made for each slot
//    with a non-zero id and given that model (`lwz r7,-4788(r31)`, then `bl 0x8341F610`). The words at stage + 10952 + 4 i and stage + 12548 + 4 i are two more slot-indexed object arrays whose
//    byte +16 the loop clears and sets (twin copies [L]); only the first array's model gets a motion player, so only it moves.
//  * The slot number is the ARRAY INDEX i [L: that it equals the arena text's slot number (entry 50001, `m(N)` rows) is a reading, the same index the motion table uses; the 399 iterations cover 0..398].
//    The model object is the join: nothing is matched by address or content. The ArenaWorld cross-check (CrossCheckArenaWorlds, a census) compares each row with the world the model-entry hook
//    recorded for the same model address in the same record, which answers the pairing and INPUTS_BOARD A1 with a count (mirror mode only: under OURS the walk is skipped and no entry exists).
//  * The world rows are the SKIN matrix of node n, derived here from the update side's own pair, never from the vtable +100 output [C wrestler_pose.md, ring.md 2.1: skin[k] = inverse_bind[k] x
//    world[k], world array at [model+192], inverse bind at [model+224]; the formula is wrestler_contrib.cpp SkinFromWorld]. n = the first entry of part 0's bone list ([model+824]: count at +8, node
//    at +12; arena_world_hook.h's rule, node 1 assumed when unreadable). The palette [model+200] is only READ, for the census: a derived skin that differs from a readable palette is counted
//    (kSfPaletteDiffer) with like compared with like.
//  * The row carries VS c76..c78 as the packer cuts them (arenaworld::RowsFromMatrix): the FULL world, the loader's own motion is dropped (ARENA_OURS.md).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../renderer_scene/rows_arena_world.h"
#include "../renderer_scene/rows_stage_fx.h"
#include "arena_world.h"   // RowsFromMatrix, RowsEqual
#include "../feed_seen.h"   // S110 FEEDREC

namespace rdr_scene::live::stagefx {

inline constexpr uint32_t kStageModels = 6164;     // stage + 6164 + 4 * slot: the slot's model object (first array)
inline constexpr uint32_t kStageMotionIds = 1224;  // stage + 1224 + 12 * slot: u16 motion id (0 = no motion player, the slot does not move)
inline constexpr uint32_t kSlotCount = 399;        // iterations of the MotionSetup loop: slots 0..398
inline constexpr uint32_t kModelPalette = 200, kModelWorldArray = 192, kModelInvBind = 224, kModelParts = 824;

// StageFxRow::flags of a kFxSlotWorld row.
enum SlotFlags : uint32_t {
  kSfNodeAssumed = 2,       // the part descriptor was not readable: node 1 assumed (as arena_world_hook.h)
  kSfPaletteDiffer = 4,     // the palette [model+200] was readable and its node n differs from the derived skin (counted: INPUTS_BOARD A1 [U])
  kSfPaletteUnread = 8,     // no palette to compare with (null / unreadable)
};

// The guest reader: copies n raw big-endian bytes, false when unreadable (inputs_core.h's ReadFn, restated here so this header stays free of it).
using ReadFn = ::wwe2k14::feedseen::ReadFnT;   // S110 FEEDREC: the plain pointer outside coverage (feed_seen.h)
inline uint32_t Swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
inline bool ReadWord(ReadFn read, uint32_t a, uint32_t& v WWE_FEED_LOC_PARAM) {
  uint32_t raw = 0;
  if (!read || !read(a, &raw, 4 WWE_FEED_LOC_ARG)) return false;
  v = Swap32(raw);
  return true;
}
inline bool ReadMat(ReadFn read, uint32_t a, float m[16]) {
  uint32_t raw[16];
  if (!read || !read(a, raw, sizeof raw)) return false;
  for (int i = 0; i < 16; ++i) {
    const uint32_t w = Swap32(raw[i]);
    std::memcpy(&m[i], &w, 4);
  }
  return true;
}
inline bool Finite16(const float m[16]) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(m[i])) return false;
  return true;
}
// skin = inverse bind x world: out row i = sum_k inv[i][k] * world.row(k) (the same formula as wrestler_contrib.cpp SkinFromWorld; sub_82296630 (out = skeleton+200, a = world, b = inverse bind)).
inline void SkinOf(const float world[16], const float inv[16], float out[16]) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += inv[i * 4 + k] * world[k * 4 + j];
      out[i * 4 + j] = s;
    }
}

struct SlotWorld {
  uint32_t slot = 0, model = 0, node = 1, flags = 0;
  float rows[12] = {};
};
enum class SlotRead { Ok, NoModel, Unreadable };

// One slot's world from its model object.
inline SlotRead ReadSlotWorld(ReadFn read, uint32_t slot, uint32_t model, SlotWorld& out) {
  out = SlotWorld{};
  out.slot = slot, out.model = model;
  if (model < 0x10000) return SlotRead::NoModel;
  uint32_t pal = 0, wld = 0, inv = 0, desc = 0, bones = 0, node = 0;
  if (!(ReadWord(read, model + kModelWorldArray, wld) && wld >= 0x10000 && ReadWord(read, model + kModelInvBind, inv) && inv >= 0x10000)) return SlotRead::Unreadable;
  if (!(ReadWord(read, model + kModelParts, desc) && desc >= 0x10000 && ReadWord(read, desc + 8, bones) && bones > 0 && ReadWord(read, desc + 12, node)))
    node = 1, out.flags |= kSfNodeAssumed;
  if (node == 0 || node >= 1024) return SlotRead::Unreadable;
  out.node = node;
  float mw[16], mi[16], skin[16];
  if (!(ReadMat(read, wld + 64u * (node - 1u), mw) && Finite16(mw) && ReadMat(read, inv + 64u * (node - 1u), mi) && Finite16(mi))) return SlotRead::Unreadable;
  SkinOf(mw, mi, skin);
  arenaworld::RowsFromMatrix(skin, out.rows);
  float mp[16];
  if (ReadWord(read, model + kModelPalette, pal) && pal >= 0x10000 && ReadMat(read, pal + 64u * (node - 1u), mp) && Finite16(mp)) {
    float rp[12];
    arenaworld::RowsFromMatrix(mp, rp);
    if (!arenaworld::RowsEqual(out.rows, rp, 1e-3f)) out.flags |= kSfPaletteDiffer;
  } else {
    out.flags |= kSfPaletteUnread;
  }
  return SlotRead::Ok;
}

// The model object of one slot (0 when the stage has none / unreadable).
inline bool ReadSlotModel(ReadFn read, uint32_t stage, uint32_t slot, uint32_t& model) {
  if (slot >= kSlotCount) return false;
  return ReadWord(read, stage + kStageModels + 4u * slot, model);
}
// The motion id of one slot (u16 at the start of the 12-byte record; the word read is big-endian, the id is its high half).
inline bool ReadSlotMotion(ReadFn read, uint32_t stage, uint32_t slot, uint32_t& id) {
  uint32_t w = 0;
  if (slot >= kSlotCount || !ReadWord(read, stage + kStageMotionIds + 12u * slot, w)) return false;
  id = w >> 16;
  return true;
}

struct SlotCensus {
  uint64_t frames = 0, rows = 0, no_model = 0, no_motion = 0, unreadable = 0, palette_unread = 0, node_assumed = 0, differ = 0, no_stage = 0, rows_dropped_cap = 0;
  uint64_t join_equal = 0, join_differ = 0, join_none = 0;   // CrossCheckArenaWorlds
};

// The rows of one frame: every `moving` slot (the arena text's m(N) slots) whose motion id is non-zero and whose model reads. `stage` = 0 -> nothing is known (counted no_stage, no row).
// The model address goes into StageFxRow::pad (kFxSlotWorld only: the cross-check's key; appliers ignore it); seq = the frame.
inline void BuildSlotRows(ReadFn read, uint32_t stage, const std::vector<uint16_t>& moving, uint32_t frame, std::vector<StageFxRow>& out, SlotCensus& c, uint32_t cap = 256) {
  ++c.frames;
  if (stage == 0) {
    ++c.no_stage;
    return;
  }
  for (uint16_t s : moving) {
    uint32_t model = 0, motion = 0;
    SlotWorld w;
    if (!ReadSlotMotion(read, stage, s, motion) || !ReadSlotModel(read, stage, s, model)) {
      ++c.unreadable;
      continue;
    }
    if (motion == 0) {   // the loop gave this slot no motion player: it does not move (also what a wrong stage object looks like)
      ++c.no_motion;
      continue;
    }
    const SlotRead r = ReadSlotWorld(read, s, model, w);
    if (r == SlotRead::NoModel) {
      ++c.no_model;
      continue;
    }
    if (r == SlotRead::Unreadable) {
      ++c.unreadable;
      continue;
    }
    if (out.size() >= cap) {
      ++c.rows_dropped_cap;
      continue;
    }
    StageFxRow row;
    row.kind = kFxSlotWorld;
    row.slot = uint16_t(s);
    row.seq = frame;
    row.flags = w.flags;
    row.pad = model;
    std::memcpy(row.world, w.rows, sizeof row.world);
    out.push_back(row);
    ++c.rows;
    if (w.flags & kSfPaletteUnread) ++c.palette_unread;
    if (w.flags & kSfNodeAssumed) ++c.node_assumed;
    if (w.flags & kSfPaletteDiffer) ++c.differ;
  }
}

// Joins each kFxSlotWorld row to the kScArenaWorlds entry (kAwObject) of the SAME model address in the same record (the LAST such entry: the table is appended oldest frame first). equal / not equal /
// no entry. Mirror mode: the walk runs, the model-entry hook records the world the packer cut; ours reads the same node's skin at the latch. Under OURS there are no entries (join_none).
inline void CrossCheckArenaWorlds(const std::vector<StageFxRow>& rows, size_t first, const std::vector<ArenaWorld>& worlds, SlotCensus& c, float tol = 1e-3f) {
  for (size_t i = first; i < rows.size(); ++i) {
    if (rows[i].kind != kFxSlotWorld) continue;
    const ArenaWorld* hit = nullptr;
    for (size_t k = worlds.size(); k-- > 0;)
      if (worlds[k].kind == kAwObject && worlds[k].object == rows[i].pad) { hit = &worlds[k]; break; }
    if (!hit) ++c.join_none;
    else if (arenaworld::RowsEqual(rows[i].world, hit->world, tol)) ++c.join_equal;
    else ++c.join_differ;
  }
}

}  // namespace rdr_scene::live::stagefx
