// S111 CHAMBERLIGHT2 (docs\research\s111\CHAMBERLIGHT2.md): the light the Elimination Chamber's category-3 pieces draw under: the one their draw worker installed LAST before them.
// Pure header (assets\asset_model.h for LightSet, the record's row types, scene_live\char_draw_list.h for the list layout, weapon_rows.h for the row flags). Reader: rf_props.cpp ApplyWeaponRows.
// Unit: tools\weapons\chamber_inherit_test.cpp (tools\weapons\chamber_inherit_selftest.py). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C chain, CHAMBERLIGHT + CHAMBERLIGHT2]: a light install copies 896 bytes of the light object into the calling thread's render record (sub_82BCC018) and nothing resets it. The chamber's
// category-3 node sub_826F8958 asks the light manager for set 35 (+0x44) and 36 (+0xAC); no retail arena key holds either, so it installs nothing and its pieces (k 0, 2, 4, 6, 7) draw under
// whatever its worker installed last. Category 3 is on worker 0 after 0..2 (range table 0x83625190); in category 1 the rim map is built after the shadow module (sub_827F6468 +0x1C / +0x2C) and
// its walk sub_827E97A0 (rim + 92 = 1) draws the bodies 13..21 and 42, then lists 33 and 43 (41 is not walked). Each character's translucent node (DRTR, base class slot 22 sub_8259FAE8 +0x100)
// installs the character's own light object (char + 3936) when it passes its gates, so the last install is the light of the LAST character in lists 33 then 43 whose DRTR draw passed them. In the
// record that is the largest kSlDrawPlace place (scene_live\cat33_places.h ReadPlaces: place = list k x kMaxNodes + word i) of a character row outside list 41. Its light object is the one the latch
// synthesizes as the character's LightState (wrestler_light_latch.h: block char + 3984 of the embedded object char + 3936), the same state rf_wrestlers.cpp lights that character with.
// A list row is a character when it has a light ref at the frame's stamp (the latch's synthetic refs are one per slot character): any other non-CWeapon node (the People's Elbow pad ELBW,
// sub_825D9D78 +0x30, appended to 33 at the move's start) draws nothing and installs nothing, so it is skipped.
// NOT in the rows (CHAMBERLIGHT2 review), so the last LISTED character is taken: the DRTR gates of sub_8259FAE8: char + 3888 == 0, char + 224 / + 232 non-zero, and, in the match look
// ([0x836F3A34] == 0) outside a shadow pass, char + 1148 == 0 (+0xD0..+0xF4), plus a sphere test for char + 3720 == 4. S111 CULLFLAG: the char + 1148 gate never fires on retail. Its one
// setter, the shadow fit's slot walk sub_82814A08, writes it only when the walk's flag word has bit 0x4, and every caller of sub_82815398 passes 1, 2 or 24, so char + 1148 keeps its reset 0
// (an off-screen cull the 360 never switches on: nothing to carry). +224 / +232 are the latch's SlotSkel::game_shown words; no live non-zero +3888 store on a character was found.
// Kind 2 / kind 4 characters (slot 22 sub_833683F8 / sub_829D2750) install the light set at [char + 5088] (sub_827F6910), not char + 3936 [C]; which scenes build them is [U].
// [L] arenas 0 / 1 / 52 / 53 hold a slot-cycle group in category 3 (sub_824E82A0 +0x2F4: its first slot has no `a`) that installs set 500 (sub_83420CB0 0x83420D4C, sub_83420EA0 +0x17C) when it draws;
// if it is listed before the chamber's node the chamber takes 500 there. Not carried (CHAMBERLIGHT2 MARGIN).
// The CDTB / CDTA rows (categories 35 / 36, the own-node class) inherit the arena's stage group `a` last light run instead [L]: not carried here (Inherit::LateNodes, the caller keeps its rule).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "../assets/asset_model.h"
#include "../scene_live/char_draw_list.h"
#include "light_install.h"
#include "rows_lights.h"
#include "rows_stage_look.h"
#include "weapon_rows.h"

namespace rdr_scene::chamber_light {

// Which install a chamber row inherits: LastCharacter = a piece the category-3 node drew; LateNodes = one CDTB / CDTA drew (weapon_rows.h kStageLrOwnNode, stage_node_state.h); None = not a chamber row.
enum class Inherit : uint8_t { None = 0, LastCharacter, LateNodes };
inline constexpr Inherit InheritOf(int32_t pach, uint32_t slot_flags) {
  if (pach != weapons::kChamberPach || !weapons::IsStageCall(slot_flags)) return Inherit::None;
  return (weapons::IsStageOwn(slot_flags) && weapons::StageLrClass(slot_flags) == weapons::kStageLrOwnNode) ? Inherit::LateNodes : Inherit::LastCharacter;
}

// Test only (tools\weapons\chamber_inherit_test.cpp --plant): 1 = the rule before CHAMBERLIGHT2 (no inherited light: the arena props set), 2 = the FIRST listed character instead of the last,
// 3 = every non-CWeapon list row taken for a character (the rule before the CHAMBERLIGHT2 review). The game never sets it.
inline int g_plant_chamber_light = 0;

// The skeleton of the character whose translucent node the rim walk draws last (lists 33 then 43), 0 = none: no character row, or a CWeapon node is listed after it (that node installs its own object [L]).
// `is_char(addr)`: the row's address is a character (LastCharacterSet: a light ref at the stamp); a row that is neither a character nor a CWeapon node installs nothing and is skipped.
template <class IsChar>
inline uint32_t LastRimCharacter(const std::vector<StageLookRow>& rows, IsChar&& is_char) {
  namespace cdl = scene_inputs::char_draw_list;
  uint32_t best = 0, first = 0;
  int32_t best_place = -1, first_place = -1, prop_place = -1;
  for (const StageLookRow& r : rows) {
    if (r.kind != kSlDrawPlace || r.rig_slot == 0 || r.dir[1] >= 0.5f) continue;   // not a list row (a body row has dir[1] = 1)
    const int32_t place = int32_t(r.rig_slot) - 1;
    if (uint32_t(place) / cdl::kMaxNodes == 1) continue;   // list 41 (k = 1): the rim walk skips it (sub_827E97A0 walks 33 and 43)
    if (r.dir[0] >= 0.5f) {   // a CWeapon node (prop)
      if (place > prop_place) prop_place = place;
      continue;
    }
    if (g_plant_chamber_light != 3 && !is_char(r.flags)) continue;   // not a character (the ELBW pad's node): it installs nothing
    if (place > best_place) best = r.flags, best_place = place;
    if (first_place < 0 || place < first_place) first = r.flags, first_place = place;
  }
  if (g_plant_chamber_light == 2) return first;
  return (best_place >= 0 && prop_place < best_place) ? best : 0u;
}

// The usable light state of skeleton `skel` at the frame's stamp (rf_wrestlers.cpp WrestlerLightState's join: the first ref of (object, stamp), then LightStateUsable), null = none.
inline const LightState* StateOf(const std::vector<LightRef>& refs, const std::vector<LightState>& states, uint32_t skel, bool have_stamp, uint32_t stamp) {
  if (!skel || !have_stamp) return nullptr;
  for (const LightRef& r : refs)
    if (r.object == skel && r.frame == stamp && r.state < states.size()) return LightStateUsable(states[r.state].light) ? &states[r.state] : nullptr;
  return nullptr;
}

// A light set holding a state's lights: the fields rf_wrestlers.cpp ApplyWrestlers copies for a character lit by its state (directions, colours + lobe weights, the key, sky, ground; no falloff; the key world space).
inline assets::LightSet SetOfState(const LightState& s) {
  assets::LightSet n;
  n.id = -1;
  n.edit_id = -1;   // a copy: the lighting editor never edits it
  const float* l = s.light;
  for (int k = 0; k < 4; ++k) {
    std::memcpy(n.dir[k], l + kLsDir + 4 * k, 12);
    std::memcpy(n.col[k], l + kLsCol + 4 * k, 12);
    n.wt[k] = l[kLsCol + 4 * k + 3];
    n.fall[k] = 0.f;
  }
  std::memcpy(n.hl_dir, l + kLsDir + 16, 12);
  std::memcpy(n.hl_col, l + kLsCol + 16, 12);
  std::memcpy(n.sky, l + kLsBlockA, 12);
  std::memcpy(n.ground, l + kLsBlockB, 12);
  n.hl_view = false;
  n.has_pcol = false;
  return n;
}

// The set a category-3 chamber row draws under: the light state of the last character the rim walk drew. false = none known (the caller keeps its rule: the arena props set).
inline bool LastCharacterSet(const std::vector<StageLookRow>& rows, const std::vector<LightRef>& refs, const std::vector<LightState>& states, bool have_stamp, uint32_t stamp, assets::LightSet& out) {
  if (g_plant_chamber_light == 1 || !have_stamp) return false;
  const auto is_char = [&](uint32_t addr) {   // a character = an object with a light ref at the stamp (StateOf's own join)
    for (const LightRef& r : refs)
      if (r.object == addr && r.frame == stamp) return true;
    return false;
  };
  const LightState* s = StateOf(refs, states, LastRimCharacter(rows, is_char), have_stamp, stamp);
  if (!s) return false;
  out = SetOfState(*s);
  return true;
}

}  // namespace rdr_scene::chamber_light
