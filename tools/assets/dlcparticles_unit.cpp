// S111 DLCPARTICLES unit (docs\research\s111\DLCPARTICLES.md): an entrance's particle bank (/PCE6/%04d in prtcls) and its scene objects (/GMEV/%04d in gm)
// are read the way the game's pac list answers: a pack's prtcls_dlcPP / gm_dlcPP before the disc's, the highest pack id first (particles_load.h FindBank).
// 0 game runs, no game files: the fixture is a temp tree of synthetic pacs (our own bytes) under out\dlcparticles_unit_fixture.
//   dlcparticles_unit.exe                     the checks
//   dlcparticles_unit.exe --plant disc_only   the old disc-only read for the whole run: must exit 1 and FAIL pack_shadows_disc
// Build and run: pwsh -NoProfile -File tools\assets\build_dlcparticles_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/particles_load.h"

namespace fs = std::filesystem;
using assets::Bytes;

namespace {

int g_fail = 0;
void Check(const char* name, bool ok, const std::string& what) {
  std::printf("%s  %s: %s\n", ok ? "PASS" : "FAIL", name, what.c_str());
  if (!ok) ++g_fail;
}

void PutLe32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }

// A PACH (pac.h ParsePach): "PACH", LE32 count, 12-byte rows {id, offset from the first data byte, size}, then the data.
Bytes MakePach(const std::vector<std::pair<uint32_t, Bytes>>& ents) {
  Bytes b(8 + 12 * ents.size(), 0);
  std::memcpy(b.data(), "PACH", 4);
  PutLe32(b, 4, uint32_t(ents.size()));
  size_t off = 0;
  for (size_t i = 0; i < ents.size(); ++i) {
    PutLe32(b, 8 + 12 * i, ents[i].first), PutLe32(b, 8 + 12 * i + 4, uint32_t(off)), PutLe32(b, 8 + 12 * i + 8, uint32_t(ents[i].second.size()));
    off += ents[i].second.size();
  }
  for (const auto& e : ents) b.insert(b.end(), e.second.begin(), e.second.end());
  return b;
}

// A PRTC holding one emitter named `name` (particles_load.h ParsePrtc: a "rootchk" block, the name at +16, max particles at +32).
Bytes MakePrtc(const char* name) {
  Bytes b(16 + 64, 0);
  std::memcpy(b.data(), "PRTC", 4);
  std::memcpy(b.data() + 16, "rootchk", 8);
  std::memcpy(b.data() + 16 + 16, name, std::min<size_t>(std::strlen(name), 15));
  b[16 + 32] = 40;
  return b;
}

// One bank: effect 0 = a nested PACH whose entry 0 is the PRTC of one emitter `name` (the emitter's name tells which file the bank came from).
Bytes MakeBank(const char* name) { return MakePach({{0u, MakePach({{0u, MakePrtc(name)}})}}); }

struct Row {
  const char* section;
  const char* id;
  Bytes pach;
};
// A plain EPAC (the pack layout, not XCTD): the first section's name at 0x800, 12-byte rows from 0x80C (a non-digit row opens a section), each
// bank's PACH at the next 2 KB sector after 0x4000 (particles_load.h EpacRecord).
void WriteEpac(const fs::path& p, const std::vector<Row>& rows) {
  Bytes f(0x4000, 0);
  std::memcpy(f.data(), "EPAC", 4);
  std::string cur = rows.empty() ? std::string("PCE6") : std::string(rows[0].section);
  std::memcpy(f.data() + 0x800, cur.c_str(), 4);
  size_t o = 0x80C;
  for (const Row& r : rows) {
    if (cur != r.section) {
      cur = r.section;
      std::memcpy(f.data() + o, cur.c_str(), 4);
      PutLe32(f, o + 4, 9);
      o += 12;
    }
    const size_t at = f.size();
    Bytes pach = r.pach;
    pach.resize((pach.size() + 0x7FF) & ~size_t(0x7FF), 0);
    f.insert(f.end(), pach.begin(), pach.end());
    std::memcpy(f.data() + o, r.id, 4);
    PutLe32(f, o + 4, uint32_t((at - 0x4000) / 0x800)), PutLe32(f, o + 8, uint32_t(pach.size()));
    o += 12;
  }
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

int Run(bool plant) {
  assets::particles::PacksSkippedPlant() = plant;
  const fs::path base = fs::absolute("out/dlcparticles_unit_fixture");
  std::error_code ec;
  fs::remove_all(base, ec);

  // prtcls: 0107 on the disc and in packs 4 and 5 (pack 5 also has a PCCH 0107, which is not a PCE6 bank); 0419 in pack 4 only; 0200 on the disc only
  WriteEpac(base / "game" / "pac" / "prtcls.pac", {{"PCE6", "0107", MakeBank("DISC_0107")}, {"PCE6", "0200", MakeBank("DISC_0200")}});
  WriteEpac(base / "dlc" / "pack4" / "pac" / "prtcls_dlc04.pac", {{"PCE6", "0107", MakeBank("PACK4_0107")}, {"PCE6", "0419", MakeBank("PACK4_0419")}});
  WriteEpac(base / "dlc" / "pack5" / "pac" / "prtcls_dlc05.pac", {{"PCCH", "0107", MakeBank("PACK5_PCCH")}, {"PCE6", "0107", MakeBank("PACK5_0107")}});
  // gm: GMEV 0107 on the disc and in pack 5; 0419 in pack 4 only; 0200 on the disc only
  WriteEpac(base / "game" / "pac" / "gm.pac", {{"GMEV", "0107", MakeBank("g")}, {"GMEV", "0200", MakeBank("g")}});
  WriteEpac(base / "dlc" / "pack4" / "pac" / "gm_dlc04.pac", {{"GMEV", "0419", MakeBank("g")}});
  WriteEpac(base / "dlc" / "pack5" / "pac" / "gm_dlc05.pac", {{"GMEV", "0107", MakeBank("g")}});

  launcher::GameFiles files;
  files.SetRoot(base / "game");
  files.SetOverlays({{base / "dlc" / "pack4", "_dlc04", "dlc", nullptr}, {base / "dlc" / "pack5", "_dlc05", "dlc", nullptr}});   // ASCENDING (the viewer's order): the rule must not depend on it

  assets::particles::Pacs cache;
  auto emitter = [&](int bank) {
    std::string err;
    const assets::particles::Bank b = assets::particles::LoadBank(files, bank, err, cache);
    if (!b.ok || b.effects.empty() || b.effects.begin()->second.emitters.empty()) return std::string("(none: ") + err + ")";
    return b.effects.begin()->second.emitters[0].name;
  };
  const std::string e107 = emitter(107), e419 = emitter(419), e200 = emitter(200), e300 = emitter(300);
  Check("pack_shadows_disc", e107 == "PACK5_0107", "PCE6 0107 (disc, pack 4, pack 5; pack 5 also PCCH 0107) -> '" + e107 + "', want the highest pack's PACK5_0107");
  Check("pack_only_bank_found", e419 == "PACK4_0419", "PCE6 0419 (pack 4 only: a DLC wrestler's bank) -> '" + e419 + "', want PACK4_0419");
  Check("disc_only_bank_stays_disc", e200 == "DISC_0200", "PCE6 0200 (disc only) -> '" + e200 + "', want DISC_0200");
  Check("missing_bank_none", e300.rfind("(none", 0) == 0, "PCE6 0300 (no file has it) -> '" + e300 + "', want none");

  auto gm_rel = [&](int bank) {
    std::string err;
    return assets::particles::FindBank(files, "gm", "GMEV", bank, cache, err).rel;
  };
  const std::string g107 = gm_rel(107), g419 = gm_rel(419), g200 = gm_rel(200);
  Check("gmev_pack_shadows_disc", g107 == "pac\\gm_dlc05.pac", "GMEV 0107 (disc, pack 5) -> '" + g107 + "', want pac\\gm_dlc05.pac");
  Check("gmev_pack_only_found", g419 == "pac\\gm_dlc04.pac", "GMEV 0419 (pack 4 only) -> '" + g419 + "', want pac\\gm_dlc04.pac");
  Check("gmev_disc_only_stays_disc", g200 == "pac\\gm.pac", "GMEV 0200 (disc only) -> '" + g200 + "', want pac\\gm.pac");
  // the seam's other reader instantiated with the same cache (its synthetic GMEV holds no objects: empty, no fault)
  const assets::particles::SceneLights sl = assets::particles::LoadSceneLights(files, 107, cache);
  Check("scene_lights_reads_pack_bank", sl.Empty(), "LoadSceneLights 0107 on the pack's (object-free) GMEV bank -> empty");

  fs::remove_all(base, ec);
  std::printf("%s (%d failed)\n", g_fail ? "dlcparticles_unit FAILED" : "dlcparticles_unit passed", g_fail);
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc >= 3 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "disc_only");
  if (argc >= 2 && !plant) {
    std::fprintf(stderr, "usage: dlcparticles_unit [--plant disc_only]\n");
    return 2;
  }
  return Run(plant);
}
