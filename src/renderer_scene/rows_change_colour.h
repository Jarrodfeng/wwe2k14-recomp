// S108 lane THR (docs\research\s108\THR_plan.md; src\scene_live\change_colour.h): the record section that carries the Superstar Threads editor's live colours
// from the draw hook to the draw.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//  67 kScChangeColour = ChangeColourRow[]  one row per wrestler body the game draws with its change-colour row this frame (model+604 set, the Threads callback).
// Optional: a reader that does not know it skips it, and a record without it draws every body with its plain row (never a failure).
#pragma once
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kScChangeColour = 67;   // 66 = kScBoneTags
inline constexpr uint32_t kChangeColourLayout = 1;
enum ChangeColourFlags : uint32_t {
  kCcUnsettled = 1,   // this frame's draw had no colours sent (editor state != 10): the colours are the last state-10 read (change_colour.h UNSETTLED)
  kCcProp = 2,        // S108 THB: the row is an entrance PROP's (object = the prop model's address; colours[0..11] = 4 RGB triples, the rest 0; the mask = the prop's /SPMT/<id> Mask1), not a wrestler body's
  kCcEditor = 4,      // S108 THB (with kCcProp): read from the Threads editor's draw callback: the game feeds MatDifCol (1, 1, 1, 1) with it; clear = a saved set's colours (the match hook), MatDifCol of the file
};
inline constexpr uint32_t kCcKnownFlags = kCcUnsettled | kCcProp | kCcEditor;
struct ChangeColourRow {   // 64 bytes
  uint32_t layout = kChangeColourLayout;
  uint32_t object = 0;          // the body's engine object (the skeleton = model address the game drew: the key of every wrestler row)
  uint32_t flags = 0;           // ChangeColourFlags
  uint8_t colours[48] = {};     // 16 RGB triples in the game's order: mask 1 r, g, b, a, mask 2 ... (g_fChangeColor[i] = byte / 255)
  uint32_t reserved = 0;
};
static_assert(sizeof(ChangeColourRow) == 64, "ChangeColourRow is a flat POD table");

}  // namespace rdr_scene
