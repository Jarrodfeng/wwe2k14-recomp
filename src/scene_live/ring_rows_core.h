// The ring's per-call node rows: the pure rules (Session 101, lane 194 = Ring D3 pieces R0-R2; docs\renderer\SKIP_GAME_WORK_MAP.md 16.3 / 16.7, docs\systems\ring.md Part D).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; NO game, SDK or asset includes: the tracker (ring_rows_hook.h), the contributor (ring_rows_contrib.h)
// and the offline test (tools\ring\ring_rows_test.cpp) all run THESE functions.
//
// INERT: nothing here changes what the game does. kRingSkipBuilt is false (no skipping is built); the switch --wwe2k14_rdr_ring_d3 only turns the record on (mode 1 = the
// verify / census arm, with or without --wwe2k14_rdr_ring_d3_verify). Marks: [C] read in a listing (cited), [L] a reading, [U] unknown.
//
//  * Ring objects. The model address of a call of sub_82C04DF0 is a ring object when it is one of the pointers of the 384-byte object at [0x836F4C74] (G): G+20 rope hi, +24 rope lo,
//    +32 the plane-shadow proxy, +36/+40 cover hi/lo, +44 turnbuckle, +48/+52 post hi/lo, +56 canvas, +332/+336/+340/+344 the four skirts (ring.md Part A section 1, stores
//    0x824EB6DC-0x824EBD58 of sub_824EB648) [C]. The alt ring (arena ids 200-249; G+68..+112) does not exist in any arena and is not handled.
//  * Node count. sub_82BD3AB8 (vtable 0x8206AF90 slot 2, image words) [C listing 0x82BD3AB8-0x82BD3AF0]: [this+296] != 0 -> the word at [this+300]; else [this+220] == 0 -> -1; else the same
//    slot 2 of the object at [this+220]. The forward is followed only when that object's vtable slot 2 is the same function (read, not assumed); anything else is "count unknown".
//  * Rows. The packer cuts VS c76..c78 of node n from the 4x4 at r5 + 64 (n - 1) and the bone slots c79.. the same way: one 64-byte matrix per node, 12 floats of rows per node
//    (arenaworld::RowsFromMatrix) [C listing 0x82C0A038-0x82C0A090 of sub_82C04DF0]. The ring reader copies node 1..N from r5 AT THE CALL: a rope side's three heights share one r5 buffer
//    (X + 29184 + 8192 * side, addi r23,r31,29344 @0x825CF5D4, addi r4,r23,-160 @0x825CFB24, addi r23,r23,8192 @0x825CFC40 [C]) that the game rewrites between heights.
//  * Registers past a mesh's palette count. A ring part's bone slots c76 + 3 * npal .. c138 hold the PREVIOUS draw's rows in the stream's bank (ring.md Part D "Stale bone slots" [C]); a
//    record has nothing for them and they are never read by the shader (every used slot < npal): "don't care", never a mismatch.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "../renderer_scene/scene_record.h"
#include "arena_world.h"  // RowsFromMatrix

namespace rdr_scene::live::ringrows {

inline constexpr bool kRingSkipBuilt = false;          // lane 194: no skipping is built (SKIP_GAME_WORK_MAP.md 16.7); the record is observed only
inline constexpr uint32_t kTableGlobal = 0x836F4C74;   // G = the word here; the ring's model pointers are at G + offset (ring.md Part A section 1)
inline constexpr uint32_t kNodeCountFn = 0x82BD3AB8;   // vtable slot 2 of the skinned model: the node count
inline constexpr uint32_t kModelAlphaOffset = 252;     // [model+252] = the f1 the vtable slot 13 passes to sub_82C04DF0 (0x82C13D80: `sub_82C04DF0(this, 0, [this+200], [this+252])`) [C listing, L as MdlAlpha]
inline constexpr uint32_t kPaletteOffset = 200;        // [model+200] = the model's own palette (r5 of its own submit)
inline constexpr uint32_t kFirstRowReg = 76, kLastRowReg = 138;   // VS c76..c138: bone 0's world rows, then 20 bone slots of 3 rows

struct ClassEntry {
  uint32_t offset;   // byte offset into G
  uint8_t cls;       // RingClass
};
inline constexpr ClassEntry kClassTable[] = {
    {20, kRcRopeHi}, {24, kRcRopeLo}, {32, kRcProxy}, {36, kRcCoverHi}, {40, kRcCoverLo}, {44, kRcTurnbuckle}, {48, kRcPostHi},
    {52, kRcPostLo}, {56, kRcCanvas}, {332, kRcSkirt},  {336, kRcSkirt},  {340, kRcSkirt}, {344, kRcSkirt},
};
inline constexpr uint32_t kClassTableCount = uint32_t(sizeof kClassTable / sizeof kClassTable[0]);

inline const char* ClassName(uint8_t c) {
  static const char* const k[kRcCount] = {"none", "rope_hi", "rope_lo", "proxy", "cover_hi", "cover_lo", "turnbuckle", "post_hi", "post_lo", "canvas", "skirt"};
  return c < kRcCount ? k[c] : "?";
}

// The pieces as the colour pass counts them (SKIP_GAME_WORK_MAP.md 16.1: post 4, turnbuckle 12, cover 12, rope 12, canvas 1, skirt objects 3 of 4). Model calls a frame, per group.
enum RingGroup : uint8_t { kRgPost = 0, kRgTurnbuckle, kRgCover, kRgRope, kRgCanvas, kRgSkirt, kRgOther, kRgCount };
inline uint8_t GroupOf(uint8_t cls) {
  switch (cls) {
    case kRcPostHi: case kRcPostLo: return kRgPost;
    case kRcTurnbuckle: return kRgTurnbuckle;
    case kRcCoverHi: case kRcCoverLo: return kRgCover;
    case kRcRopeHi: case kRcRopeLo: return kRgRope;
    case kRcCanvas: return kRgCanvas;
    case kRcSkirt: return kRgSkirt;
    default: return kRgOther;
  }
}
inline const char* GroupName(uint8_t g) {
  static const char* const k[kRgCount] = {"post", "turnbuckle", "cover", "rope", "canvas", "skirt", "other"};
  return g < kRgCount ? k[g] : "?";
}
inline constexpr uint32_t kExpectedCalls[kRgCount] = {4, 12, 12, 12, 1, 3, 0};   // [C: 16.1 and ring.md Part D, Check B / 7079 / h1 / J1 / wm30; the skirt set is 3 of 4 by camera quadrant]

// The class table as read from G: pointers[i] belongs to kClassTable[i]; a pointer of 0 is "not made".
struct RingTable {
  uint32_t global = 0;
  uint32_t ptr[kClassTableCount] = {};
  bool valid = false;
  uint8_t ClassOf(uint32_t model) const {
    if (!valid || model == 0) return kRcNone;
    for (uint32_t i = 0; i < kClassTableCount; ++i)
      if (ptr[i] == model) return kClassTable[i].cls;
    return kRcNone;
  }
};
// Reads G and the pointers. false = G unreadable or null (no ring in this scene).
template <class ReadWordFn>
inline bool ReadRingTable(ReadWordFn read_word, RingTable& out) {
  out = RingTable{};
  uint32_t g = 0;
  if (!read_word(kTableGlobal, g) || g < 0x10000) return false;
  out.global = g;
  for (uint32_t i = 0; i < kClassTableCount; ++i) {
    uint32_t p = 0;
    out.ptr[i] = read_word(g + kClassTable[i].offset, p) ? p : 0;
  }
  out.valid = true;
  return true;
}

// sub_82BD3AB8 as the packer's reader sees it (forwards followed only when verified). n = the signed result; false = could not be read (flags bit kRrCountUnknown set by the caller).
template <class ReadWordFn>
inline bool NodeCount(ReadWordFn read_word, uint32_t model, int32_t& n, uint32_t& flags) {
  uint32_t obj = model;
  for (int depth = 0; depth < 4; ++depth) {
    uint32_t vt = 0, fn = 0, f296 = 0, f300 = 0, f220 = 0;
    if (!read_word(obj, vt) || !read_word(vt + 8, fn) || fn != kNodeCountFn) return false;   // slot 2 must BE the function this rule describes
    if (!read_word(obj + 296, f296)) return false;
    if (f296 != 0) {
      if (!read_word(obj + 300, f300)) return false;
      n = int32_t(f300);
      return true;
    }
    if (!read_word(obj + 220, f220)) return false;
    if (f220 == 0) {
      n = -1;
      return true;
    }
    obj = f220;
    flags |= kRrCountForwarded;
  }
  return false;
}

// Is register c<reg> of a ring part a bone slot past the mesh's palette count? Row registers are c76..c138; palette slot k (0-based; slot 0 = c76..c78, the instance's own transform) occupies c76 + 3k..c76 + 3k + 2
// (tools\ring\ring_ledger.py group_of: `reg >= 76 + 3 * npal` is 'stale').
// Registers c76 + 3 * npal .. c138 are don't care: the stream's bank keeps the previous draw's rows there and no record carries them. npal 0 is treated as "unknown" (nothing is don't care).
inline bool RowsRegisterDontCare(uint32_t npal, uint32_t reg) {
  if (npal == 0 || reg < kFirstRowReg || reg > kLastRowReg) return false;
  return reg >= kFirstRowReg + 3 * npal;
}

// The comparison register -> (slot, row) of a row register; false outside c76..c138.
inline bool RowRegisterSlot(uint32_t reg, uint32_t& slot, uint32_t& row) {
  if (reg < kFirstRowReg || reg > kLastRowReg) return false;
  slot = (reg - kFirstRowReg) / 3, row = (reg - kFirstRowReg) % 3;
  return true;
}

// The rows of one record node as 3 vec4: node rows (12 floats) of the entry, or nullptr when the entry holds fewer nodes than the slot's node asks for.
inline const float* NodeRowsOf(const std::vector<RingNodeRows>& pool, const RingRows& e, uint32_t node_1based) {
  if (node_1based == 0 || node_1based > e.n_nodes || uint64_t(e.first_node) + node_1based > pool.size()) return nullptr;
  return pool[e.first_node + node_1based - 1].rows;
}

// The join between a frame's ring entries and the LightRefs of the same record: the ring entry's `call` is the LightRef ordinal, so per (frame, object) the number of entries
// must equal the number of refs. This is the live verify arm's own cross-check (nothing but the two readers' agreement; the stream compare is ring_rows_check.py on the zip).
struct RefCross {
  uint64_t frames_joined = 0, objects = 0, equal = 0, differ = 0, unjoined_frames = 0;
};
inline void CrossCheckLightRefs(const std::vector<RingRows>& rows, size_t first, const std::vector<LightRef>& refs, uint32_t frame, RefCross& acc) {
  bool any_ref = false;
  for (const LightRef& r : refs)
    if (r.frame == frame) { any_ref = true; break; }
  if (!any_ref) { ++acc.unjoined_frames; return; }
  ++acc.frames_joined;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> calls;   // (object, the entries' `call` values)
  for (size_t i = first; i < rows.size(); ++i) {
    if (rows[i].frame != frame) continue;
    bool f = false;
    for (auto& c : calls)
      if (c.first == rows[i].object) { c.second.push_back(rows[i].call); f = true; break; }
    if (!f) calls.push_back({rows[i].object, {rows[i].call}});
  }
  for (auto& c : calls) {
    std::vector<uint32_t> ords;   // the LightRef ordinals of that object in that frame
    for (const LightRef& r : refs)
      if (r.frame == frame && r.object == c.first) ords.push_back(r.ordinal);
    std::sort(c.second.begin(), c.second.end());
    std::sort(ords.begin(), ords.end());
    ++acc.objects;
    if (ords == c.second) ++acc.equal;   // the same SET of ordinals (counts alone would let a shifted ordinal through)
    else ++acc.differ;
  }
}

}  // namespace rdr_scene::live::ringrows
