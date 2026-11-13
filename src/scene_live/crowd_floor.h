// The seated floor people's op-21 observer (Session 101, lane 82; docs\renderer\CROWD_FAMILY_8276ED60.md sections 2.1 and 3.1). Copyright (c) the wwe2k14-recomp
// authors. BSD-3-Clause.
//
// The floor people are drawn by sub_8276EAA0 (db floor_seat_group_draw), inside the crowd draw entry sub_82519508's bracket: per visible person one op 21
// (bl 0x82BB27D8 at 0x8276ED38, return address 0x8276ED3C) records c76-c78, then the draw (op 15 at 0x8276ED60). This file is the PURE decode of that call's
// registers (listing 0x8276EAA0-0x8276EDB0, lane 78 section 2.1) into a crowd::MemberObs: no game or SDK includes, so tools\scene\crowd_make.cpp unit-tests it
// with a fake memory (the plants: a wrong group, a type that disagrees with the element, an LOD other than 0). The game side (crowd_hooks.h) supplies the memory.
//
//   r26 = the group object: +20 member count, +24 the person type, +64 the member element array (176 bytes each: +23 the type byte, +27 row, +28 seat)
//   r29 = the member index i (0 .. count-1), r31 = 28 * g (g = 20 * LOD + type; mulli at 0x8276EC18), r4 = 76, r6 = 3, r5 = the 12 constants
// Every register meaning is from the listing ([C] in lane 78); that these reads give the right values on a RUNNING game is the live check (the hook's register reads
// on a new capture): until then a rejected call is counted (FloorStats), never recorded.
#pragma once
#include <cstdint>

#include "crowd_contrib.h"

namespace rdr_scene::live::crowd {

inline constexpr uint32_t kFloorOp21Return = 0x8276ED3C;  // after bl 0x82BB27D8 at 0x8276ED38
inline constexpr uint32_t kFloorGroupCount = 20;          // g % 20 == type (g = 20 * LOD + type)

enum class FloorReject : uint8_t { None = 0, Regs, Type, Group, Lod, Index, Element };

// What the observer saw, for the log line / the live check (any thread: plain counters read on the writer's thread are approximate by design).
struct FloorStats {
  uint64_t seen = 0, taken = 0, rejected[7] = {};
};

// `Mem` supplies u32(addr), u8(addr), f32(addr) for guest memory (big-endian already undone).
template <class Mem>
inline FloorReject DecodeFloorMember(uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r26, uint32_t r29, uint32_t r31, Mem& mem, MemberObs& out) {
  if (r4 != 76 || r6 != 3) return FloorReject::Regs;
  const uint32_t type = mem.u32(r26 + 24);
  if (type >= kTypes) return FloorReject::Type;
  if (r31 % 28 != 0) return FloorReject::Group;
  const uint32_t g = r31 / 28;
  if (g >= kTypes * kLods || g % kFloorGroupCount != type) return FloorReject::Group;
  if (g / kFloorGroupCount != 0) return FloorReject::Lod;  // the floor draw reads [manager+592] = 0: LOD 0 only [C]; anything else is not understood, not recorded
  const uint32_t count = mem.u32(r26 + 20);
  if (r29 >= count) return FloorReject::Index;
  const uint32_t elements = mem.u32(r26 + 64);
  if (!elements || mem.u8(elements + 176 * r29 + 23) != type) return FloorReject::Element;
  out = MemberObs{};
  out.block = type, out.lod = 0, out.index = r29, out.floor = true;
  for (uint32_t k = 0; k < 12; ++k) out.c[k] = mem.f32(r5 + 4 * k);
  return FloorReject::None;
}

}  // namespace rdr_scene::live::crowd
