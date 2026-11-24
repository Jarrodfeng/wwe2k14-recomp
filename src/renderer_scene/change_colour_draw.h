// S108 lane THR2 (docs\research\s108\THR_plan.md section 6; src\scene_live\change_colour.h): the draw side of the Superstar Threads editor's live colours.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game, SDK or renderer includes: rf_wrestlers.cpp, front_end.cpp and the offline unit
// (tools\scene\thr_unit.cpp, stage "handoff") run THIS code.
//
// The game's draw of a recoloured body [C, listings; our words]: model+604 != 0 -> each material's effect from model+392 (the second registry, EDIT/STS = edit.pac PACH 9),
// then the callback sub_8339F0E0 sends g_fChangeColor[0..15] = (R, G, B, 1) / 255 and binds Mask1..Mask4 at pixel samplers 7, 4, 14, 2. Ours, in production order:
//   1. rf_wrestlers ApplyWrestlers: the record's section-67 row of the body's engine object -> SceneFrame::change_colours (CarryChangeColour: the 16 constants, once per body).
//   2. front_end Record: a wrestler draw of that body asks the bind for the change-colour row (WantsChangeColour: the material has its masks, change_mask.h) ...
//   3. ... ChooseCharEffectTable(change_colour) picks the STS table's row, BindMaterial binds the masks by RoleOfChangeSlot (MaskTextureOfSlot), and ResolveChar fills
//      ChangeColor from the constants (CharCtx::change_color).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../assets/asset_model.h"
#include "../scene_live/change_colour.h"   // ToConstants, MaskIndexOfSlot (pure)
#include "rows_change_colour.h"

namespace rdr_scene::change_colour_draw {

constexpr int kFloats = live::change_colour::kColours * 4;   // 64: ChangeColor[16]

// The draw side's census (printed with the store's line, wrestler_contrib.cpp SlotCensus).
struct DrawCensus {
  std::atomic<uint64_t> carried{0};      // rows carried into a frame (one per recoloured body drawn)
  std::atomic<uint64_t> no_engine{0};    // a row whose body is not in this frame's wrestler objects (not drawn this frame)
  std::atomic<uint64_t> draws{0};        // wrestler draws bound to the change-colour row (masks bound, ChangeColor fed)
  std::atomic<uint64_t> no_masks{0};     // a recoloured body's draw whose material has no masks (the PACH has none: the game's [E+80] = 0 rule) -> plain
  std::atomic<uint64_t> no_row{0};       // the STS table has no row for the effect / it could not be bound -> the startup row, plain
  std::atomic<uint64_t> no_table_pick{0};   // the wrestler table pick is off (wwe2k14_rdr_char_table_pick): no change colour at all
  std::atomic<uint64_t> unnamed{0};      // a recoloured body the record could not name (guest rows: no masks can be loaded)
  std::string Text() const {
    const auto u = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
    return "[scene][change_colour] drawn: rows carried " + u(carried) + " (not in the frame " + u(no_engine) + "), draws " + u(draws) + "; plain: no masks " + u(no_masks) + ", no STS row " + u(no_row) +
           ", table pick off " + u(no_table_pick) + ", unnamed body " + u(unnamed);
  }
};
inline DrawCensus& Draws() {
  static DrawCensus d;
  return d;
}

// The record's row of a body by its engine object (the skeleton = model address); null = the body is not recoloured this frame.
inline const ChangeColourRow* RowOfEngine(const std::vector<ChangeColourRow>& rows, uint32_t engine) {
  for (const ChangeColourRow& r : rows)
    if (r.object == engine) return &r;
  return nullptr;
}

// Step 1 (rf_wrestlers, where the body's engine object is joined): the row -> the frame's entry for the body at `frame_index` (its index in SceneFrame::objects).
// `Frame` = SceneFrame (a template so this header stays free of front_end.h).
template <class Frame>
inline bool CarryChangeColour(const std::vector<ChangeColourRow>& rows, Frame& f, uint32_t frame_index, uint32_t engine) {
  const ChangeColourRow* r = RowOfEngine(rows, engine);
  if (!r) return false;
  typename decltype(f.change_colours)::value_type e;
  e.object = frame_index, e.flags = r->flags;
  live::change_colour::ToConstants(r->colours, e.constants);
  f.change_colours.push_back(e);
  Draws().carried.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Step 2 (front_end, per draw): the body's 16 constants by its frame index, null = none.
template <class Vec>
inline const float* ConstantsOf(const Vec& v, uint32_t frame_index) {
  for (const auto& e : v)
    if (e.object == frame_index) return e.constants;
  return nullptr;
}

// A material can be drawn recoloured only with its masks (change_mask.h KeepMasks set them on a `/m` load).
inline bool HasMasks(const assets::Material& m) {
  for (int k = 0; k < live::change_colour::kColours / 4; ++k)
    if (m.change_mask[k] >= 0) return true;
  return false;
}
// The draw asks for the change-colour row: the body is recoloured this frame and the material has its masks. Counted when it cannot.
inline bool WantsChangeColour(const float* constants, const assets::Material& m) {
  if (!constants) return false;
  if (!HasMasks(m)) { Draws().no_masks.fetch_add(1, std::memory_order_relaxed); return false; }
  return true;
}

// S108 THB: an entrance PROP's row rides the same section / carry (kCcProp: colours[0..11], the mask = the prop's SPMT Mask1 in Material::change_mask[0]; rf_props.cpp ApplyWeaponRows carries it at the prop's frame index).
// The entry of a draw's object (null = not recoloured); the caller reads its flags (kCcProp / kCcEditor) and constants.
template <class Vec>
inline const typename Vec::value_type* EntryOf(const Vec& v, uint32_t frame_index) {
  for (const auto& e : v)
    if (e.object == frame_index) return &e;
  return nullptr;
}
inline bool IsPropEntry(uint32_t flags) { return (flags & kCcProp) != 0; }
inline bool PropHasMask(const assets::Material& m) { return m.change_mask[0] >= 0; }   // the prop's SPMT Mask1 is loaded (live_assets.cpp ApplyPropMask)

// Step 3 (BindMaterial): the texture index bound at a change-colour draw's pixel slot `slot` (Mask1..Mask4 at 7, 4, 14, 2); -1 = not a mask slot or no such mask (black).
inline int MaskTextureOfSlot(const assets::Material& m, int slot) {
  const int k = live::change_colour::MaskIndexOfSlot(slot);
  return k < 0 ? -1 : m.change_mask[k];
}

}  // namespace rdr_scene::change_colour_draw
