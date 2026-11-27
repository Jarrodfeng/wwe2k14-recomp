// S111 GEARLIQUID unit: which character draws take their body's liquid map at pixel sampler 14 (g_mBSSPrm), through the real liquid_gate::TakesBodyMap
// (src\renderer_scene\liquid_gate.h, called by FrontEnd::Record) and the real role chain muscle_map::Role (FrontEnd::BindMaterial's switch: its LiquidMap
// case stores Bound::liquid_slot) over the real registry and define tables. Synthetic data only, 0 game runs (docs\research\s111\GEARLIQUID.md).
//   out\tools\gearliquid_unit.exe   selftest; the planted twins: the old gate (yCh_skin* only) must leave a gear blood draw on the black stand-in, and the
//   landed gate without the body-mesh test (GEARLIQUID review) must give a merged piece mesh (src_key != 0) its owner's map
// Build + run: pwsh -NoProfile -File tools\scene\build_gearliquid_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_scene/fx_tables.h"
#include "../../src/renderer_scene/liquid_gate.h"
#include "../../src/renderer_scene/muscle_map.h"

using namespace rdr_scene;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-52s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

// The bind's liquid slot for a table-picked fragment, as ChooseCharEffectTable and BindMaterial make it: wet = the fragment samples 14 and the row is not
// the Threads editor's; the role switch (muscle_map::Role) gives LiquidMap at the slot it stores.
uint8_t LiquidSlotOf(const rdr::fx::Variant& v, bool change_colour, bool muscle) {
  bool wet = false;
  for (uint32_t s = 0; s < v.sampler_count; ++s) wet = wet || v.samplers[s].slot == 14;
  if (change_colour) wet = false;
  uint8_t slot = liquid_gate::kNoSlot;
  for (uint32_t s = 0; s < v.sampler_count; ++s)
    if (muscle_map::Role(int(v.samplers[s].slot), change_colour, wet, muscle) == CharSlot::LiquidMap) slot = v.samplers[s].slot;
  return slot;
}

// FrontEnd::Record's image at the liquid slot: the body's map when the gate passes and the body has a published map this frame, else the bind's black stand-in.
constexpr int kBlack = 7, kMap = 77;
int Slot14(bool gate, bool body_has_map) { return gate && body_has_map ? kMap : kBlack; }

// The planted twin: Record's gate before the lane (yCh_skin* only).
bool OldGate(const std::string& effect, bool is_char, int material, uint8_t liquid_slot) {
  return is_char && material >= 0 && effect.rfind("yCh_skin", 0) == 0 && liquid_slot != liquid_gate::kNoSlot;
}

// The planted twin of the review fix: the gate as first landed (no body-mesh test), which a stock piece merged into the body's object passed.
bool LandedGate(bool is_char, int material, uint8_t liquid_slot, int /*src_key*/) { return is_char && material >= 0 && liquid_slot != liquid_gate::kNoSlot; }

int Selftest() {
  // ---- the gate on synthetic binds
  Check(liquid_gate::TakesBodyMap(true, 0, 14, 0), "gate.char_wet_bind_takes_map", "a body mesh's character draw whose bind holds the liquid slot");
  Check(!liquid_gate::TakesBodyMap(true, 0, liquid_gate::kNoSlot, 0), "gate.dry_bind_none", "no liquid slot: nothing to replace");
  Check(!liquid_gate::TakesBodyMap(false, 0, 14, 0), "gate.non_character_none", "a stage / prop draw never takes a body map");
  Check(!liquid_gate::TakesBodyMap(true, -1, 14, 0), "gate.no_material_none", "a mesh with no material");
  // GEARLIQUID review: a SUBM piece (its own model in the game, no callback; merged into the body's object by LoadWrestlerPac) takes no map
  for (const int piece : {26, 27, 28, 10000, 20002, 21000})
    Check(!liquid_gate::TakesBodyMap(true, 0, 14, piece), ("gate.piece_" + std::to_string(piece) + "_none").c_str(), "a piece mesh: the game's slot 14 there is stale, not its owner's map");

  // ---- every character fragment of the registry, by its define list
  std::unordered_map<std::string, const fxt::VariantTable*> named;
  for (const fxt::VariantTable& t : fxt::kTables) named.emplace(t.variant, &t);
  int gear_blood = 0, gear_blood_bad = 0, skin14 = 0, skin14_moved = 0, reads14 = 0, reads14_bad = 0, dry_gated = 0, cc_gated = 0, cc14 = 0;
  for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
    if (!v.name || std::strncmp(v.name, "yCh_", 4) != 0) continue;
    const size_t n = std::strlen(v.name);
    if (n < 9 || std::strcmp(v.name + n - 9, "_fragment") != 0) continue;
    const auto it = named.find(v.name);
    if (it == named.end()) continue;
    const char* defs = it->second->defines;
    const bool mu = muscle_map::ReadsMuscle(defs);
    const bool ccol = muscle_map::HasDefine(defs, "FX_CHANGE_COLOR") || muscle_map::HasDefine(defs, "FX_CAMO");   // only the Threads table binds these (change_colour_table)
    bool has14 = false;
    for (uint32_t s = 0; s < v.sampler_count; ++s) has14 = has14 || v.samplers[s].slot == 14;
    const std::string name = v.name;
    const std::string effect = name.substr(0, name.find('_', 4));   // yCh_gear / yCh_skin / ... (the flex twins share the prefix)
    if (ccol) {
      cc14 += has14 ? 1 : 0;
      cc_gated += liquid_gate::TakesBodyMap(true, 0, LiquidSlotOf(v, true, mu), 0) ? 1 : 0;
      continue;
    }
    const uint8_t ls = LiquidSlotOf(v, false, mu);
    const bool gate = liquid_gate::TakesBodyMap(true, 0, ls, 0);
    if (!has14) { dry_gated += gate ? 1 : 0; continue; }
    ++reads14;
    reads14_bad += (gate && ls == 14) ? 0 : 1;
    if (effect == "yCh_gear" && muscle_map::HasDefine(defs, "FX_BLOOD")) {
      ++gear_blood;
      gear_blood_bad += (gate && Slot14(gate, true) == kMap && Slot14(gate, false) == kBlack) ? 0 : 1;
    }
    if (effect == "yCh_skin") {
      ++skin14;
      skin14_moved += gate == OldGate(name, true, 0, ls) ? 0 : 1;
    }
  }
  Check(gear_blood > 0 && gear_blood_bad == 0, "registry.gear_blood_fragments_take_body_map",
        std::to_string(gear_blood) + " yCh_gear* FX_BLOOD fragments: slot 14 = the body map when published, black when not");
  Check(reads14 > 0 && reads14_bad == 0, "registry.every_wet_bind_takes_slot_14", std::to_string(reads14) + " character fragments sample 14 (not change-colour)");
  Check(skin14 > 0 && skin14_moved == 0, "registry.skin_unchanged", std::to_string(skin14) + " skin fragments sample 14: the same answer as the old gate");
  Check(dry_gated == 0, "registry.dry_fragments_untouched", "a fragment that samples no slot 14 never takes a map");
  Check(cc14 > 0 && cc_gated == 0, "registry.change_colour_keeps_mask3", std::to_string(cc14) + " change-colour / camo fragments sample 14: Mask3 there, never the map");

  // ---- dry / wet for one gear blood draw (question 4: a dry body = no published map = the black stand-in = exactly the draw before the lane)
  Check(Slot14(liquid_gate::TakesBodyMap(true, 3, 14, 0), false) == kBlack, "image.dry_body_black", "no map published (nothing painted since the clear)");
  Check(Slot14(liquid_gate::TakesBodyMap(true, 3, 14, 0), true) == kMap, "image.bloodied_body_map", "the body's map once painted");
  Check(Slot14(liquid_gate::TakesBodyMap(true, 3, 14, 28), true) == kBlack, "image.bloodied_piece_black", "a piece mesh of a bloodied body keeps the black stand-in");

  // ---- the planted twin: the old yCh_skin-only gate leaves a gear blood draw black with a painted body map
  const int twin = Slot14(OldGate("yCh_gear_shadow_blood_blood_bump_fragment", true, 3, 14), true);
  Check(twin == kBlack, "twin.old_gate_caught", "the old gate gives image " + std::to_string(twin) + " (black), not the body map " + std::to_string(kMap));
  // ---- the planted twin of the review fix: the landed gate gives a merged piece mesh (Mick Foley's entry-28 yCh_gear sock) its owner's map
  const int twin2 = Slot14(LandedGate(true, 3, 14, 28), true);
  Check(twin2 == kMap, "twin.landed_gate_caught", "the landed gate gives the piece image " + std::to_string(twin2) + " (the owner's map), the fixed gate black");

  std::printf("%s gearliquid_unit selftest: a character draw whose bind reads slot 14 takes its body's liquid map (skin and gear)\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main() { return Selftest(); }
