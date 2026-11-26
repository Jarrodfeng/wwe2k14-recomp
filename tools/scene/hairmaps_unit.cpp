// S111 HAIRMAPS unit: the hair effects' noise / shift maps at pixel samplers 8 / 9 (g_mHairNoisePrm / g_mHairShiftPrm, name-table rows 0x8392ABC0 / 0x8392ABE0),
// through the real RoleOfSlot / RoleOfWetSlot / RoleOfChangeSlot / muscle_map::Role (src\renderer_scene\char_effects.cpp, muscle_map.h: what FrontEnd::BindMaterial calls)
// over the real registry, and the real loader (LoadWrestler -> LoadJboy). 0 game runs (docs\research\s111\HAIRMAPS.md).
//   out\tools\hairmaps_unit.exe                   selftest: the roles of slots 8 / 9 on every bind kind, the image each takes, the planted twin (the old chain: grey at
//                                                 8 / 9) must bind the wrong image
//   out\tools\hairmaps_unit.exe real --game DIR   read-only, the player's files: ch103 (hair_noise / hair_shift in its own pac) and the first ch177 attire whose
//                                                 hair names noise2 (chCommon.pac only) through the production loader
// Build + run: pwsh -NoProfile -File tools\scene\build_hairmaps_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../src/assets/asset_load.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"
#include "../../src/renderer_scene/muscle_map.h"

using namespace rdr_scene;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

// Images as BindMaterial's switch hands them out for slots 8 / 9 (front_end.cpp: HairNoise -> mat.hair_noise_map, HairShift -> mat.hair_shift_map, else grey_).
constexpr int kNone = 0, kGrey = 5;
int Slot89(CharSlot role, const assets::Material& m) {
  const auto tex = [](int i) { return i >= 0 ? 100 + i : kNone; };
  switch (role) {
    case CharSlot::HairNoise: return tex(m.hair_noise_map) != kNone ? tex(m.hair_noise_map) : kGrey;
    case CharSlot::HairShift: return tex(m.hair_shift_map) != kNone ? tex(m.hair_shift_map) : kGrey;
    default: return -1;
  }
}
// The twin: BindMaterial before the lane: slots 8 / 9 were RoleOfSlot's FlatGrey (grey_, whatever the material names).
int OldSlot89(const assets::Material&) { return kGrey; }

int Selftest() {
  // ---- the roles: slot 8 = the noise map, 9 = the shift map, on every bind kind (dry, wet, change-colour; muscle fragment or not)
  int wrong = 0;
  for (int cc = 0; cc < 2; ++cc)
    for (int wet = 0; wet < 2; ++wet)
      for (int mu = 0; mu < 2; ++mu) {
        wrong += muscle_map::Role(8, cc != 0, wet != 0, mu != 0) == CharSlot::HairNoise ? 0 : 1;
        wrong += muscle_map::Role(9, cc != 0, wet != 0, mu != 0) == CharSlot::HairShift ? 0 : 1;
      }
  Check(wrong == 0, "role.slot8_noise_slot9_shift_every_bind", "change-colour / wet / muscle, all 8 combinations through muscle_map::Role");
  Check(RoleOfSlot(8) == CharSlot::HairNoise && RoleOfSlot(9) == CharSlot::HairShift && RoleOfWetSlot(8) == CharSlot::HairNoise && RoleOfWetSlot(9) == CharSlot::HairShift &&
            RoleOfChangeSlot(8) == CharSlot::HairNoise && RoleOfChangeSlot(9) == CharSlot::HairShift,
        "role.the_three_role_functions", "RoleOfSlot / RoleOfWetSlot / RoleOfChangeSlot");
  Check(RoleOfSlot(2) == CharSlot::FlatBlack && RoleOfSlot(5) == CharSlot::WrinkleNormal, "role.neighbours_unchanged", "2 / 5 keep their roles");

  // ---- the registry: which character fragments sample 8 / 9 (informative: a material that names no hair map keeps grey there)
  int read8 = 0, read9 = 0;
  std::string owners;
  for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
    if (!v.name || std::strncmp(v.name, "yCh_", 4) != 0) continue;
    const size_t n = std::strlen(v.name);
    if (n < 9 || std::strcmp(v.name + n - 9, "_fragment") != 0) continue;
    bool h8 = false, h9 = false;
    for (uint32_t s = 0; s < v.sampler_count; ++s) h8 = h8 || v.samplers[s].slot == 8, h9 = h9 || v.samplers[s].slot == 9;
    read8 += h8 ? 1 : 0, read9 += h9 ? 1 : 0;
    if (h8 || h9) {
      const std::string nm(v.name);
      const std::string own = nm.compare(0, 15, "yCh_Aniso_trans") == 0 ? "yCh_Aniso_trans" : nm.compare(0, 15, "yCh_Anisotropic") == 0 ? "yCh_Anisotropic" : nm;
      if (owners.find(own) == std::string::npos) owners += (owners.empty() ? "" : ", ") + own;
    }
  }
  Check(read8 > 0 && read9 > 0, "registry.fragments_sample_8_and_9", std::to_string(read8) + " read 8, " + std::to_string(read9) + " read 9: " + owners);

  // ---- the image slots 8 / 9 take
  assets::Material hair, none;
  hair.shader = none.shader = "yCh_Aniso_trans";
  hair.hair_noise_map = 3, hair.hair_shift_map = 4;
  Check(Slot89(RoleOfSlot(8), hair) == 103 && Slot89(RoleOfSlot(9), hair) == 104, "bind.hair_material_takes_its_maps", "slot 8 = Model::textures[3], slot 9 = [4]");
  Check(Slot89(RoleOfSlot(8), none) == kGrey && Slot89(RoleOfSlot(9), none) == kGrey, "bind.no_hair_map_grey", "a material naming none (or a texture that did not load): today's grey");
  assets::Material dflt;
  Check(dflt.hair_noise_map == -1 && dflt.hair_shift_map == -1, "material.default_none", "-1 until the loader reads g_mHairNoisePrm / g_mHairShiftPrm");

  // ---- the planted twin: the old chain binds grey at a hair draw's 8 / 9
  const int twin = OldSlot89(hair);
  Check(twin != 103, "twin.old_chain_caught", "the old chain gives image " + std::to_string(twin) + " (grey), not the material's 103");

  std::printf("%s hairmaps_unit selftest: slots 8 / 9 of a hair draw = the material's g_mHairNoisePrm / g_mHairShiftPrm maps\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

struct HairCount { int hair = 0, noise = 0, shift = 0, bad = 0; std::string noise_name, shift_name; int noise_fmt = -1; };
HairCount CountHair(const assets::Model& m) {
  HairCount c;
  for (const assets::Material& mt : m.materials) {
    if (Lower(mt.shader) != "ych_aniso_trans") continue;
    ++c.hair;
    if (mt.hair_noise_map >= 0) {
      ++c.noise;
      if (size_t(mt.hair_noise_map) < m.textures.size()) c.noise_name = Lower(m.textures[size_t(mt.hair_noise_map)].name), c.noise_fmt = int(m.textures[size_t(mt.hair_noise_map)].format);
      else ++c.bad;
    }
    if (mt.hair_shift_map >= 0) {
      ++c.shift;
      if (size_t(mt.hair_shift_map) < m.textures.size()) c.shift_name = Lower(m.textures[size_t(mt.hair_shift_map)].name);
      else ++c.bad;
    }
  }
  return c;
}

// The player's files, read-only.
int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  // ch103's hair (yCh_Aniso_trans, SpeCol1 (1.8, 1.8, 1.8, 1)) names hair_noise / hair_shift, textures of its own model: the first of its attires that holds hair
  {
    bool loaded = false;
    HairCount c;
    int at = -1;
    const int n103 = assets::AttireCount(files, 103);
    for (int a = 0; a < n103 && a < 16 && c.hair == 0; ++a) {
      assets::Model m;
      std::string err;
      if (!assets::LoadWrestler(files, 103, a, m, err)) continue;
      loaded = true, c = CountHair(m), at = a;
    }
    Check(loaded, "real.load_ch103", std::to_string(n103) + " attires");
    Check(c.hair > 0 && c.noise > 0 && c.shift > 0 && c.bad == 0 && c.noise_name == "hair_noise" && c.shift_name == "hair_shift", "real.ch103_hair_carries_its_maps",
          "attire " + std::to_string(at) + ": " + std::to_string(c.hair) + " yCh_Aniso_trans materials: " + std::to_string(c.noise) + " noise ('" + c.noise_name + "', format " + std::to_string(c.noise_fmt) + "), " +
              std::to_string(c.shift) + " shift ('" + c.shift_name + "')");
  }
  // ch177's hair names noise2, a texture only chCommon.pac holds: the loader's shared directories (asset_load.h, added last) answer it
  bool found = false;
  std::string note = "no ch177 attire's hair names noise2";
  const int attires = assets::AttireCount(files, 177);
  for (int a = 0; a < attires && a < 16 && !found; ++a) {
    assets::Model m;
    std::string err;
    if (!assets::LoadWrestler(files, 177, a, m, err)) continue;
    const HairCount c = CountHair(m);
    if (c.noise_name == "noise2") found = true, note = "attire " + std::to_string(a) + ": " + std::to_string(c.noise) + " of " + std::to_string(c.hair) + " hair materials take noise2";
  }
  Check(found, "real.ch177_noise2_from_chcommon", note);
  std::printf("%s hairmaps_unit real\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: hairmaps_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  return Selftest();
}
