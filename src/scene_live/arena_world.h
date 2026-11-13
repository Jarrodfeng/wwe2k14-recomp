// The arena's evaluated worlds and clock: pure functions (Session 101, lane 130; docs\renderer\ARENA_OURS.md "D3 gaps: placed world, moving world, clock").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only; NO game, SDK or asset includes: the hook (arena_world_hook.h), the contributor
// (arena_world_contrib.h) and the offline oracle (tools\scene\arena_world_oracle.cpp) all run THESE functions.
//
// Sources, with their marks (the evidence is in ARENA_OURS.md; [C] measured on Check B / run 7079 / h1 / checkF, [L] reading, [U] unknown):
//  * Matrix -> rows. The packer cuts VS c76..c78 of a part from node n of [model+200]: it loads the 4x4 (64 bytes) at palette + 64 * (n - 1) and transposes it
//    with vmrghw / vmrglw before storing 48 bytes [C listing 0x82C0A038-0x82C0A090 of sub_82C04DF0]. Row k of the rows = column k of the matrix as the game keeps it
//    (row-vector 4x4, translation in m[12..14]): (m[k], m[4+k], m[8+k], m[12+k]). RowsFromMatrix is that.
//  * Placed slots 390-397 [C]: a pure translation, the same in every frame of three captures of arena 4; (561, 0, -560), (-561, 0, -560), (532.9, 0, 560), (-532.9, 0, 560)
//    for (slot - 390) & 3 = 0..3. The same table is in src\assets\asset_load.h (matched by S97 on arena 5's probes). Where the game computes it is [U]; it is not a
//    literal in the arena pac (no float 561 or 532.9 in any of the 464 entries of bg04.pac). Only arenas 4 and 5 are [C]; any other arena is "unknown".
//  * The clock. VS c186 `Time` of the UV-scrolling surfaces is a counter that advances 1 per guest frame (the 1x group; the beam cones run 2x) and is NOT the host frame
//    counter: g - c186 = 2203 / 2197 / 2195 on run 7079 / h1 / checkF [C], and all three captures end on c186 = 2040. Its guest source is [U]. The shipped form is
//    clock = frame - origin with origin = the guest frame the stage was set up in (CStage::MotionSetup exit): a hypothesis the J1 capture decides.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rdr_scene::live::arenaworld {

constexpr uint32_t kFirstPlaced = 390, kLastPlaced = 397;

// The 12 floats VS c76..c78 take from a 4x4 palette matrix (16 floats, the game's layout, host order).
inline void RowsFromMatrix(const float m[16], float out[12]) {
  for (int k = 0; k < 3; ++k)
    for (int r = 0; r < 4; ++r) out[4 * k + r] = m[4 * r + k];
}
// The inverse (for the offline oracle: rows -> a matrix whose translation is in m[12..14]).
inline void MatrixFromRows(const float rows[12], float m[16]) {
  for (int k = 0; k < 3; ++k)
    for (int r = 0; r < 4; ++r) m[4 * r + k] = rows[4 * k + r];
  m[3] = m[7] = m[11] = 0.f;
  m[15] = 1.f;
}

inline bool IsPlacedSlot(uint32_t slot) { return slot >= kFirstPlaced && slot <= kLastPlaced; }
// Arenas whose placed table is measured: 4 (three captures, every frame) and 5 (S97 probe run 6728).
inline bool PlacedArenaKnown(int arena) { return arena == 4 || arena == 5; }

// The placed slots' world rows; false when the slot is not a placed slot or the arena's table is not measured (unknown stays unknown).
inline bool PlacedRows(int arena, uint32_t slot, float out[12]) {
  if (!IsPlacedSlot(slot) || !PlacedArenaKnown(arena)) return false;
  static const float kx[4] = {561.f, -561.f, 532.9f, -532.9f}, kz[4] = {-560.f, -560.f, 560.f, 560.f};
  const uint32_t c = (slot - kFirstPlaced) & 3u;
  const float rows[12] = {1, 0, 0, kx[c], 0, 1, 0, 0, 0, 0, 1, kz[c]};
  std::memcpy(out, rows, sizeof rows);
  return true;
}

// clock = frame - origin on the drawing thread's frame counter (low 32 bits both, so a wrap is harmless); false when the origin is not known.
inline bool ClockAt(uint32_t frame, uint32_t origin, bool origin_known, float& out) {
  if (!origin_known) return false;
  out = float(int32_t(frame - origin));
  return true;
}

// The J1 comparison, shared by the oracle and by arena_ready's "recorded" source: rows equal within tolerance (a float32 chain through the matrix keeps bits, so 1e-4 is generous).
inline bool RowsEqual(const float a[12], const float b[12], float tol = 1e-4f) {
  for (int i = 0; i < 12; ++i)
    if (!(std::fabs(a[i] - b[i]) <= tol)) return false;
  return true;
}

// A table keyed by the HOOK's frame stamp (never by a record index: map section 8). Entries are ArenaWorld-shaped but the template keeps this header free of the record's.
// Returns the first entry of (frame, object, kind) or nullptr (the clock entry, kind 2, is per frame: the object is ignored). A consumer that joins at another frame
// than the draw's (the plant stamp_off_by_one) gets the neighbour frame's value.
template <class Entry>
inline const Entry* FindEntry(const Entry* tab, size_t n, uint32_t frame, uint32_t object, uint8_t kind) {
  for (size_t i = 0; i < n; ++i)
    if (tab[i].frame == frame && tab[i].kind == kind && (kind != 1 || tab[i].object == object)) return &tab[i];
  return nullptr;
}

}  // namespace rdr_scene::live::arenaworld
