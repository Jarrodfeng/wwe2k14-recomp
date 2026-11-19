// S103 DLW: offline proof that the live loader's file reader sees the packs the game was offered (src\dlc_overlays.h, src\renderer_scene\wrestler_src.h) and
// that a failed named wrestler load is remembered (src\renderer_scene\named_load_fail.h). 0 game runs; the fixture is a temp tree of fake files (nothing from the game).
//   dlw_unit.exe selftest                       the fixture checks + the planted twins (the old behaviour must be caught)
//   dlw_unit.exe real --game DIR --dlc DIR      read-only, the player's own install: the DLC folder's packs resolve ch385 through the overlay and the loader loads it
// Build: pwsh -NoProfile -File tools\scene\build_dlw_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/dlc_catalog.h"
#include "../../src/dlc_overlays.h"
#include "../../src/renderer_scene/named_load_fail.h"
#include "../../src/renderer_scene/wrestler_src.h"

namespace fs = std::filesystem;
using namespace rdr_scene;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}
static void Put(const fs::path& p, const char* bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << bytes;
}

static int SelfTest() {
  const fs::path base = fs::absolute("out/dlw_unit_fixture");
  std::error_code ec;
  fs::remove_all(base, ec);
  const fs::path game = base / "game", dlc4 = base / "dlc" / "DLC Pack 3", mod6 = base / "mods" / "superstars" / "pack6";
  Put(game / "pac" / "ch" / "ch001.pac", "DISC-001");
  Put(game / "default.xex", "x");
  Put(dlc4 / "pac" / "ch" / "ch385_dlc04.pac", "DLC-385");     // a pack names its file the disc's name + "_dlcPP"
  Put(dlc4 / "pac" / "dlc_mot04.pac", "MOT");                  // a name of its own: tried literally
  Put(mod6 / "pac" / "ch" / "ch400_dlc06.pac", "MOD-400");     // the imported-superstars pack
  Put(mod6 / "pac" / "ch" / "ch385_dlc06.pac", "MOD-385");     // the same id as the DLC pack's: the HIGHEST pack id wins (the game's rule, catalog_dlc.md item 3)

  launcher::GameFiles files;
  files.SetRoot(game);
  uint64_t gen = ~0ull;

  // The old behaviour (the planted twin): no overlays published, the live loader's reader sees the disc only: ch385 does not exist.
  wwe2k14::dlc_overlays::Publish({});
  wrestler_src::ApplyOverlays(files, &gen);
  Check(files.Exists("pac\\ch\\ch001.pac") && !files.Exists("pac\\ch\\ch385.pac") && !std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch385.pac"), "none"),
        "PLANT (the old reader: root only): pac\\ch\\ch385.pac does not open -> the DLC wrestler was a hole");

  // The fix: the game's offered packs are published (src\dlc.h Install) and the reader applies them.
  wwe2k14::dlc_overlays::Publish({{mod6, "_dlc06", "mod", 6}, {dlc4, "_dlc04", "dlc", 4}});   // dlc.h publishes DESCENDING pack ids
  Check(wrestler_src::ApplyOverlays(files, &gen), "a new published set is applied");
  Check(!wrestler_src::ApplyOverlays(files, &gen), "an unchanged set is not re-applied");
  Check(files.Exists("pac\\ch\\ch385.pac"), "ch385 resolves through the overlay (pac\\ch\\ch385_dlc04.pac)");
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch385.pac"), "mod"), "precedence: the same path in packs 4 and 6 -> pack 6 (the highest id wins; census word 'mod')");
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch400.pac"), "mod"), "census word: ch400 is 'mod' (the imported-superstars pack)");
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch001.pac"), "disc"), "census word: ch001 is 'disc' (the disc first)");
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch777.pac"), "none"), "census word: a pac nobody has is 'none'");
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\dlc_mot04.pac"), "dlc"), "a pack's own-named file is found literally");
  {
    auto head = files.ReadHead("pac\\ch\\ch385.pac", 16);
    Check(std::string(head.begin(), head.end()) == "MOD-385", "ch385's bytes are pack 6's (read through the overlay)");
    std::string got;
    files.Stream("pac\\ch\\ch400.pac", [&](const uint8_t* d, size_t n) { got.append(reinterpret_cast<const char*>(d), n); });
    Check(got == "MOD-400", "Stream reads the mod pack's file");
    head = files.ReadHead("pac\\ch\\ch001.pac", 16);
    Check(std::string(head.begin(), head.end()) == "DISC-001", "a disc file still comes from the disc");
  }
  // the toggle: the mods pack is published only while its toggle is on (dlc.h offers it only then): off = the vanilla reader again
  wwe2k14::dlc_overlays::Publish({{dlc4, "_dlc04", "dlc", 4}});
  wrestler_src::ApplyOverlays(files, &gen);
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch385.pac"), "dlc"), "toggle off: ch385 is pack 4's again (census word 'dlc')");
  Check(!files.Exists("pac\\ch\\ch400.pac") && files.Exists("pac\\ch\\ch385.pac"), "mod toggle off: the superstars pack is gone from the reader, the DLC pack stays");
  wwe2k14::dlc_overlays::Publish({});
  wrestler_src::ApplyOverlays(files, &gen);
  Check(!files.Exists("pac\\ch\\ch385.pac"), "no packs offered: the reader is the disc's again");

  // S103 DLW review item 1: a pack that is an STFS PACKAGE (the phone's DLC is packages; a 360-drive dump gives packages). The package is made by our own writer (tools\dlc\make_test_stfs.py) from fake files.
  {
    const fs::path src = base / "stfs_src", pkgfile = base / "stfs" / "pack5.stfs";
    std::string big;
    for (int i = 0; i < 12000; ++i) big += char((i * 7 + 3) & 0xFF);   // three 4 KB blocks: Stream / ReadHead cross block boundaries
    Put(src / "pac" / "ch" / "ch386_dlc05.pac", big.c_str());   // (no NUL in the pattern would be needed: written below by length)
    { std::ofstream f(src / "pac" / "ch" / "ch386_dlc05.pac", std::ios::binary); f.write(big.data(), std::streamsize(big.size())); }
    Put(src / "pac" / "ch" / "ch387.pac", "LITERAL-387");
    fs::create_directories(pkgfile.parent_path());
    const std::string cmd = "py -I tools/dlc/make_test_stfs.py \"" + src.string() + "\" \"" + pkgfile.string() + "\" --name \"DLW test\" > nul 2>&1";
    Check(std::system(cmd.c_str()) == 0 && fs::exists(pkgfile), "the test STFS package is written (make_test_stfs.py)");
    // PLANT (the old skip: dlc.h left packages out): nothing published for it -> ch386 does not exist
    wwe2k14::dlc_overlays::Publish({});
    wrestler_src::ApplyOverlays(files, &gen);
    Check(!files.Exists("pac\\ch\\ch386.pac"), "PLANT (packages skipped, the old behaviour): ch386 does not resolve");
    wwe2k14::dlc_overlays::Item it;
    it.root = pkgfile, it.suffix = "_dlc05", it.kind = "dlc", it.pack_id = 5, it.package = true;
    wwe2k14::dlc_overlays::Publish({it});
    wrestler_src::ApplyOverlays(files, &gen);
    Check(files.Exists("pac\\ch\\ch386.pac") && !std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch386.pac"), "dlc"), "ch386 resolves through the STFS package overlay (ch386_dlc05.pac inside it), census word 'dlc'");
    auto head = files.ReadHead("pac\\ch\\ch386.pac", 20);
    Check(head.size() == 20 && head[0] == uint8_t(big[0]) && head[19] == uint8_t(big[19]), "ReadHead reads the package file's first bytes");
    std::string got;
    const bool sok = files.Stream("pac\\ch\\ch386.pac", [&](const uint8_t* d, size_t n) { got.append(reinterpret_cast<const char*>(d), n); });
    Check(sok && got == big, "Stream reads the whole 12000-byte file across its 4 KB blocks, byte for byte");
    head = files.ReadHead("pac\\ch\\ch387.pac", 64);
    Check(std::string(head.begin(), head.end()) == "LITERAL-387", "a literal-named file in the package resolves too");
    bool has386 = false, has387 = false;
    for (const std::string& n : files.List("pac\\ch")) has386 = has386 || n == "ch386.pac", has387 = has387 || n == "ch387.pac";
    Check(has386 && has387, "List shows the package's files under the disc's names (ch386.pac, ch387.pac)");
    {  // COV8 gap 6: PropPach / EntranceFiles / MotionFiles build their pack list from the reader's overlays (dlc_files::PackIds): the published packs are what they scan
      const std::vector<int> ids = assets::dlc_files::PackIds(files);
      Check(ids.size() == 1 && ids[0] == 5, "the prop / entrance pack list (dlc_files::PackIds) is the published pack (5), the STFS package included");
      const auto ent = assets::dlc_files::EntranceFiles(files);
      Check(std::find(ent.begin(), ent.end(), "pac/evt/Entrance_dlc05.pac") != ent.end() && assets::dlc_files::PropFile(5 * assets::dlc_files::kPropPackStride + 1) == "pac\\evt\\evtobj_dlc05.pac", "the entrance bank files and the prop file name of pack 5 follow the overlay");
    }
    Check(!files.Exists("pac\\ch\\ch999.pac") && files.Stream("pac\\ch\\ch999.pac", [](const uint8_t*, size_t) {}) == false, "a file the package lacks is missing");
    wwe2k14::dlc_overlays::Publish({});
    wrestler_src::ApplyOverlays(files, &gen);
  }

  // The named-load failure registry (RecordCanName / the emitter read it: wrestler_emit selftest carries the contributor side)
  namedfail::Clear();
  Check(!namedfail::Failed("pac\\ch\\ch385.pac", "model_key:38510", 1), "nothing failed yet");
  Check(namedfail::Note("pac\\ch\\ch385.pac", "model_key:38510", 1), "the first failure of a ref is fresh (the census line is said once)");
  Check(!namedfail::Note("PAC/CH/CH385.PAC", "model_key:38510", 1), "the same ref, other spelling: not fresh");
  Check(namedfail::Failed("pac/ch/ch385.pac", "model_key:38510", 1) && !namedfail::Failed("pac\\ch\\ch385.pac", "model_key:38520", 1), "only the ref that failed is failed");
  Check(namedfail::MaybeFailedChar(385) && !namedfail::MaybeFailedChar(100), "the per-character bit: 385 maybe failed, 100 not (the game thread's one-bit test)");
  Check(!namedfail::Note("pac\\bg\\bg002.pac", "", 0) && !namedfail::Failed("pac\\bg\\bg002.pac", "", 0), "only wrestler pacs are tracked");
  namedfail::Clear();
  Check(!namedfail::Failed("pac\\ch\\ch385.pac", "model_key:38510", 1), "Clear (a new game folder) forgets the failures");

  fs::remove_all(base, ec);
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  dlw_unit selftest\n");
  return g_fail ? 1 : 0;
}

// The player's own install, read-only: the DLC folder's ready extracted packs are published exactly as dlc.h does and the loader's own function loads ch385.
static int Real(const char* game_dir, const char* dlc_dir) {
  launcher::GameFiles files;
  files.SetRoot(fs::path(game_dir));
  uint64_t gen = ~0ull;
  wwe2k14::dlc_overlays::Publish({});
  wrestler_src::ApplyOverlays(files, &gen);
  assets::Model m0;
  std::string e0;
  const bool old_ok = assets::LoadWrestler(files, 385, 0, m0, e0);   // PLANT: the old reader (root only)
  const int old_prop = assets::PropPach(files, 2591);   // PLANT: Fandango's entrance prop (evtobj_dlc04.pac PACH 0) with no packs
  Check(old_prop == -1, "PLANT (the old reader): Fandango's entrance prop 2591 resolves to -1 (COV8 gap 6)");
  Check(!old_ok, "PLANT (the old reader, root only): LoadWrestler(385) FAILS on the player's own files");
  std::printf("      old reader error: %s\n", e0.c_str());
  std::vector<wwe2k14::dlc::Pack> packs = wwe2k14::dlc::Scan(fs::path(dlc_dir), true, false);
  std::vector<wwe2k14::dlc_overlays::Item> items;
  for (const auto& p : packs) {
    if (p.status != wwe2k14::dlc::Status::kReady) continue;
    char sfx[16];
    std::snprintf(sfx, sizeof sfx, "_dlc%02d", p.pack_id);
    items.insert(items.begin(), {p.root, sfx, "dlc", p.pack_id, p.package});   // descending pack id, as dlc.h publishes
  }
  std::printf("      %zu pack(s) scanned, %zu ready folder(s) published\n", packs.size(), items.size());
  wwe2k14::dlc_overlays::Publish(items);
  wrestler_src::ApplyOverlays(files, &gen);
  Check(files.Exists("pac\\ch\\ch385.pac") && !std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch385.pac"), "dlc"), "pac\\ch\\ch385.pac resolves through the DLC overlay (census word 'dlc')");
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 385, 0, m, err);
  Check(ok && !m.meshes.empty(), "the loader's own LoadWrestler(385, attire 0) loads the DLC wrestler (meshes > 0)");
  std::printf("      meshes %zu bones %zu%s%s\n", m.meshes.size(), m.bones.size(), ok ? "" : " error: ", ok ? "" : err.c_str());
  const int prop = assets::PropPach(files, 2591);
  Check(prop >= 0 && prop / assets::dlc_files::kPropPackStride == 4, "DLC entrance prop 2591 (Fandango) resolves to a PACH of pack 4's evtobj_dlc04.pac through the overlay");
  std::printf("      prop 2591 -> PACH %d\n", prop);
  Check(!std::strcmp(wrestler_src::Where(files, "pac\\ch\\ch100.pac"), "disc"), "a disc wrestler (ch100) is still read from the disc");
  if (g_fail) std::printf("FAIL  %d check(s) failed\n", g_fail);
  else std::printf("PASS  dlw_unit real\n");
  return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return SelfTest();
  if (argc >= 6 && !std::strcmp(argv[1], "real") && !std::strcmp(argv[2], "--game") && !std::strcmp(argv[4], "--dlc")) return Real(argv[3], argv[5]);
  std::fprintf(stderr, "usage: dlw_unit selftest | real --game DIR --dlc DIR\n");
  return 2;
}
