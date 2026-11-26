// S111 MUSCLEMAP unit: the flex effects' muscle normal map at pixel sampler 2 (g_mMusclePrm, name-table row sub_82BAAEE8 0x82BACDEC), through the real
// muscle_map::Role / Image (src\renderer_scene\muscle_map.h, called by FrontEnd::BindMaterial) over the real registry and define tables. 0 game runs
// (docs\research\s111\MUSCLEMAP.md).
//   out\tools\musclemap_unit.exe                   selftest: the roles of every character fragment, the image slot 2 takes, the planted twin (the old chain:
//                                                  black at slot 2) must bind the wrong image
//   out\tools\musclemap_unit.exe real --game DIR   read-only, the player's files: ch161 attire 0 through the production loader (LoadWrestler -> LoadJboy):
//                                                  its flex parts carry their g_mMusclePrm texture (an "mf" map)
// Build + run: pwsh -NoProfile -File tools\scene\build_musclemap_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_scene/fx_tables.h"
#include "../../src/renderer_scene/muscle_map.h"

using namespace rdr_scene;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

// The twin: BindMaterial's chain before the lane (front_end.cpp: change-colour -> RoleOfChangeSlot, wet -> RoleOfWetSlot, else RoleOfSlot); slot 2 = FlatBlack.
CharSlot OldRole(int slot, bool change_colour, bool wet) { return change_colour ? RoleOfChangeSlot(slot) : wet ? RoleOfWetSlot(slot) : RoleOfSlot(slot); }

// Images as BindMaterial's switch hands them out for slot 2 (front_end.cpp: MuscleMap -> muscle_map::Image, ChangeMask4 -> the model's mask, FlatBlack -> black).
constexpr int kNone = 0, kBlack = 7, kMask4 = 50;
int Slot2(CharSlot role, const assets::Material& m) {
  const auto tex = [](int i) { return i >= 0 ? 100 + i : kNone; };
  switch (role) {
    case CharSlot::MuscleMap: return muscle_map::Image(m, tex, kNone, kBlack);
    case CharSlot::ChangeMask4: return kMask4;
    case CharSlot::FlatBlack: return kBlack;
    default: return -1;
  }
}

int Selftest() {
  // ---- the define test
  Check(muscle_map::ReadsMuscle("FX_MUSCLE") && muscle_map::ReadsMuscle("FX_CUBE_SPECULAR FX_MUSCLE") && muscle_map::ReadsMuscle("FX_MUSCLE FX_SHADOW"),
        "defines.reads_muscle", "FX_MUSCLE alone, last, first");
  Check(!muscle_map::ReadsMuscle("FX_MUSCLES") && !muscle_map::ReadsMuscle("XFX_MUSCLE FX_SHADOW") && !muscle_map::ReadsMuscle("") && !muscle_map::ReadsMuscle(nullptr),
        "defines.whole_word_only", "a longer word, a suffix match, empty, null");

  // ---- every character fragment of the registry, by its define list
  std::unordered_map<std::string, const fxt::VariantTable*> named;
  for (const fxt::VariantTable& t : fxt::kTables) named.emplace(t.variant, &t);
  int muscle = 0, muscle_no2 = 0, muscle_cc = 0, other2 = 0, wrong = 0, others_moved = 0;
  for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
    if (!v.name || std::strncmp(v.name, "yCh_", 4) != 0) continue;
    const size_t n = std::strlen(v.name);
    if (n < 9 || std::strcmp(v.name + n - 9, "_fragment") != 0) continue;
    const auto it = named.find(v.name);
    if (it == named.end()) continue;
    const char* defs = it->second->defines;
    const bool mu = muscle_map::ReadsMuscle(defs), ccol = muscle_map::HasDefine(defs, "FX_CHANGE_COLOR");
    bool has2 = false;
    for (uint32_t s = 0; s < v.sampler_count; ++s) has2 = has2 || v.samplers[s].slot == muscle_map::kSlot;
    for (int wet = 0; wet < 2; ++wet)
      for (int s = 0; s < 16; ++s)
        if (s != muscle_map::kSlot || !mu) others_moved += muscle_map::Role(s, ccol, wet != 0, mu) == OldRole(s, ccol, wet != 0) ? 0 : 1;
    if (mu) {
      ++muscle;
      muscle_no2 += has2 ? 0 : 1;
      muscle_cc += ccol ? 1 : 0;
      for (int wet = 0; wet < 2; ++wet) wrong += muscle_map::Role(muscle_map::kSlot, false, wet != 0, true) == CharSlot::MuscleMap ? 0 : 1;
    } else if (has2) {
      ++other2;
    }
  }
  const std::string counts = std::to_string(muscle) + " FX_MUSCLE fragments (" + std::to_string(muscle_no2) + " without slot 2), " + std::to_string(other2) + " other fragments sample slot 2";
  Check(muscle > 0 && muscle_no2 == 0, "registry.muscle_fragments_sample_2", counts);
  Check(muscle_cc == 0, "registry.no_fragment_has_muscle_and_change_colour", "the MATFEED review's fx_registry rule: slot 2 is never Mask4 on a muscle fragment");
  Check(wrong == 0, "role.muscle_fragment_slot2_is_muscle_map", "dry and wet binds");
  Check(others_moved == 0, "role.every_other_slot_unchanged", "every slot of a non-muscle fragment and every slot but 2 of a muscle one keep the old chain");
  Check(muscle_map::Role(muscle_map::kSlot, true, false, true) == CharSlot::ChangeMask4, "role.change_colour_bind_keeps_mask4", "a change-colour bind's 2 stays Mask4 (sub_8339F0E0)");

  // ---- the image slot 2 takes
  assets::Material flex, none;
  flex.shader = "yCh_skin_flex", flex.muscle = 3;
  none.shader = "yCh_skin_flex";
  Check(Slot2(muscle_map::Role(2, false, false, true), flex) == 103 && Slot2(muscle_map::Role(2, false, true, true), flex) == 103, "bind.muscle_draw_takes_material_map",
        "dry and wet: Model::textures[3]");
  Check(Slot2(muscle_map::Role(2, false, false, true), none) == kBlack, "bind.no_muscle_map_black", "a material naming none: black (alpha 0, the plain normal map)");
  Check(Slot2(muscle_map::Role(2, true, false, true), flex) == kMask4, "bind.change_colour_draw_mask4", "never the muscle map on a change-colour bind");
  Check(Slot2(muscle_map::Role(2, false, false, false), flex) == kBlack, "bind.non_muscle_fragment_black", "a fragment without FX_MUSCLE keeps the black stand-in");
  assets::Material dflt;
  Check(dflt.muscle == -1, "material.default_none", "Material::muscle = -1 until the loader reads g_mMusclePrm");

  // ---- the planted twin: the old chain binds black at a muscle draw's slot 2
  const int twin = Slot2(OldRole(2, false, false), flex);
  Check(twin != 103, "twin.old_chain_caught", "the old chain gives image " + std::to_string(twin) + " (black), not the material's 103");

  std::printf("%s musclemap_unit selftest: slot 2 of an FX_MUSCLE draw = the material's g_mMusclePrm map\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

// The player's files, read-only: ch161 attire 0 through the production loader.
int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 161, 0, m, err);
  Check(ok && !m.materials.empty(), "real.load_ch161", ok ? std::to_string(m.materials.size()) + " materials" : err);
  // The file names g_mMusclePrm on non-flex parts too (ch161: yCh_skin / yCh_gear -> mf12); only the flex effects' fragments define FX_MUSCLE, so only
  // those parts ever sample it (muscle_map::Role keys on the fragment, not on the material).
  int with = 0, flex = 0, flex_with = 0, bad_name = 0, nonflex = 0;
  for (const assets::Material& mt : m.materials) {
    const bool fl = mt.shader.find("_flex") != std::string::npos;
    flex += fl ? 1 : 0;
    if (mt.muscle < 0) continue;
    ++with;
    flex_with += fl ? 1 : 0;
    nonflex += fl ? 0 : 1;
    std::string nm = size_t(mt.muscle) < m.textures.size() ? m.textures[size_t(mt.muscle)].name : std::string();
    for (char& c : nm) c = char(std::tolower((unsigned char)c));
    if (nm.rfind("mf", 0) != 0) ++bad_name;
    if (with <= 6 || nm.rfind("mf", 0) != 0) std::printf("      %s -> muscle texture '%s'\n", mt.shader.c_str(), nm.c_str());
  }
  Check(flex_with > 0 && bad_name == 0, "real.flex_parts_carry_mf_maps",
        std::to_string(with) + " materials carry a muscle map: " + std::to_string(flex_with) + " of " + std::to_string(flex) + " flex, " + std::to_string(nonflex) +
            " non-flex (never sampled); " + std::to_string(bad_name) + " not an mf map");
  std::printf("%s musclemap_unit real\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: musclemap_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  return Selftest();
}
