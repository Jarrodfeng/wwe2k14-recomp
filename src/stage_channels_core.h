// wwe2k14 - S111 TRONKEY (docs\research\s111\TRONTOOL.md F1, CAATRON.md): which titan channel each stage screen key shows, the game's two tables, stated ONCE. Portable, std only (no SDK).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game's rule [C sub_824EC3C0 = CStage::SetupTitan +1E0..+290]: for every screen key 800..814 present in the arena pac it builds one screen object (sub_82699E60) with ONE channel, picked by the key
// number alone (no material or sampler name is read), from one of two fixed tables. The created-arena table is taken when the match arena word G+64 is 200..249 (sub_822A7240 -> sub_822AA2F8).
// Channel 0 = the streamed tron (the 320x320 sheet), 1 = the panel movie (MVSW / MOV, 256x256), 2 = the wall movie (MVSB / MVAB, 256x128), -1 = the live frame grab (sub_8276A808).
//   key        800  801  802  803  804  805  806  807  808  809  810  811-814
//   shipped     -1    1    1    0    0    2    1    0    1    1   -1    -1
//   200..249    -1    1   -1    0    1   -1    2    2    0   -1   -1     1
// Read by the match renderer's screen binding (src\scene_live\screen_planes_core.h ScreenBinding::LoadDefaults / ForArena); the viewer and a Tron preview tool call the same functions (TRONTOOL F1).
#pragma once
#include <cstdint>

#include "tron_sheet_core.h"   // kSheetSize: the tron sheet's one size

namespace wwe2k14::stage_channels {

inline constexpr uint32_t kFirstKey = 800, kLastKey = 814;
inline constexpr int kGrab = -1;
inline constexpr int kTronChannel = 0;   // S111 TRONLIVE: the streamed tron (the 320x320 sheet): "a titantron plays" = a row for a key bound to this channel (StageFxContributor::Emit)
inline constexpr int kShippedTable = 0, kCreatedTable = 1;

// A Create-an-Arena venue id [C sub_822AA2F8: 1 for 200..249].
inline bool IsCreatedArena(int arena_id) { return arena_id >= 200 && arena_id <= 249; }
inline int TableOf(int arena_id) { return IsCreatedArena(arena_id) ? kCreatedTable : kShippedTable; }

// The channel screen `key` shows in arena `arena_id` (-1 = unknown arena: the shipped table); kGrab for a key with no channel.
inline int StageChannelOfKey(int arena_id, uint32_t key) {
  if (IsCreatedArena(arena_id)) {
    switch (key) {
      case 803: case 808: return 0;
      case 806: case 807: return 2;
      case 801: case 804: case 811: case 812: case 813: case 814: return 1;
      default: return kGrab;
    }
  }
  switch (key) {
    case 803: case 804: case 807: return 0;
    case 805: return 2;
    case 801: case 802: case 806: case 808: case 809: return 1;
    default: return kGrab;
  }
}

// The picture size a channel's movie gives (the size a bound key expects); false for the grab / an unknown channel.
inline bool StageChannelFrame(int channel, uint32_t& w, uint32_t& h) {
  switch (channel) {
    case 0: w = h = tron_sheet::kSheetSize; return true;   // the tron sheet (S110 TRONSHEET: the shim hands the game exactly this size)
    case 1: w = h = 256; return true;                      // the panel sheet (S103 M2, run 18)
    case 2: w = 256, h = 128; return true;                 // the wall movie (S103 lane M, run 12)
    default: return false;
  }
}

}  // namespace wwe2k14::stage_channels
