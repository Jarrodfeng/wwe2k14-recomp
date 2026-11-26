// S111 OLDCHAIN unit (Session 111, 0 game runs; docs\research\s111\OLDCHAIN.md): a wrestler (yCh_*) material is drawn ONLY with its effect-table row.
// No table = a counted HOLE and ONE loud line, never another pick; the Arena Viewer's rdr backend publishes the table from its own game files.
// The production code runs: char_effects.cpp ChooseCharEffectTable (linked), char_effects.h CharHoleOf / CharHoleCounter, effect_table_build.h
// BuildEffectTable / PublishFromViewerFiles (header).
//   out\tools\oldchain_unit.exe [--game DIR] [--plant]
//     no --game: the no-table checks (no game files needed); --game DIR (the player's extracted files): + the viewer's publish and a bound row.
//     --plant: the TEST ONLY twins of effect_table_build.h are set (g_plant_silent_no_table: a failed build says nothing; g_plant_viewer_no_publish:
//     the viewer publishes nothing); the checks they target (build.loud_line, viewer.no_files_loud, viewer.publishes) must FAIL: exit 0 only when every
//     targeted check that ran failed and no other check failed.
// Build + run: pwsh -NoProfile -File tools\scene\build_oldchain_unit.ps1. Exit 0 = pass, 1 = fail, 2 = usage.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/effect_table_build.h"
#include "../../src/renderer_scene/technique_pick.h"

using namespace rdr_scene;

namespace {

bool g_plant = false;
const std::set<std::string> kTargets = {"build.loud_line", "viewer.no_files_loud", "viewer.publishes"};
int g_bad = 0, g_caught = 0, g_missed = 0;

void Check(bool ok, const std::string& name, const std::string& what) {
  if (g_plant && kTargets.count(name)) {
    std::printf("%s %s: %s\n", ok ? "MISSED (the plant passed)" : "caught", name.c_str(), what.c_str());
    ++(ok ? g_missed : g_caught);
    return;
  }
  std::printf("%s %s: %s\n", ok ? "ok  " : "FAIL", name.c_str(), what.c_str());
  if (!ok) ++g_bad;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--plant") g_plant = true;
    else return std::printf("usage: oldchain_unit [--game DIR] [--plant]\n"), 2;
  }
  if (g_plant) read_rule::g_plant_silent_no_table.store(true), read_rule::g_plant_viewer_no_publish.store(true);

  // 1. no table published: the table pick is a hole with its reason, never a pair
  read_rule::PublishEffectTable(nullptr);
  const CharChoice nt = ChooseCharEffectTable("yCh_skin", true, false, 0, 1);
  Check(!nt.Ok() && nt.no_table && nt.vs < 0 && nt.ps < 0, "hole.no_table", "no table: vs " + std::to_string(nt.vs) + " ps " + std::to_string(nt.ps) + " (" + nt.why_not + ")");
  const char* ctr = CharHoleCounter(CharHoleOf(nt));
  Check(CharHoleOf(nt) == CharHole::NoTable && ctr && std::string(ctr) == "char_hole_no_table", "hole.counter", std::string("counted under ") + (ctr ? ctr : "(none)"));
  CharChoice wr, other, bound;
  wr.blocked_wrinkles = true, other.why_not = "a sampler slot the front end cannot feed", bound.vs = 0, bound.ps = 1;
  const char* c_row = CharHoleCounter(CharHole::NoRow);
  const char* c_wrk = CharHoleCounter(CharHole::Wrinkles);
  Check(CharHoleOf(wr) == CharHole::Wrinkles && CharHoleOf(other) == CharHole::NoRow && CharHoleOf(bound) == CharHole::None && !CharHoleCounter(CharHole::None) &&
            c_row && std::string(c_row) == "char_hole_no_row" && c_wrk && std::string(c_wrk) == "char_wrinkle_blocked",
        "hole.kinds", "a blocked wrinkle row / any other refusal / a bound row");

  // 2. a failed build (no pac\_startup.pac): nothing published, ONE loud line that names the hole counters
  std::vector<std::string> said;
  const read_rule::Say say = [&](const std::string& s) { said.push_back(s); };
  const auto loud = [&]() {
    return said.size() == 1 && said[0].find("NO EFFECT TABLE") != std::string::npos && said[0].find("char_hole_no_table") != std::string::npos &&
           said[0].find("char_hole_draws") != std::string::npos;
  };
  const auto what_said = [&]() { return said.empty() ? std::string("nothing said") : std::to_string(said.size()) + " line(s), first: " + said[0]; };
  launcher::GameFiles none;
  none.SetRoot(std::filesystem::temp_directory_path() / "oldchain_unit_no_game_here");
  const bool built = read_rule::BuildEffectTable(none, say);
  Check(!built && !read_rule::EffectTableNow(), "build.fails", "no pac\\_startup.pac: nothing published");
  Check(loud(), "build.loud_line", what_said());

  // 3. the viewer with no game files: nothing published, the same one loud line
  said.clear();
  const bool vnone = read_rule::PublishFromViewerFiles(nullptr, say);
  Check(!vnone && !read_rule::EffectTableNow(), "viewer.no_files", "no game files: nothing published");
  Check(loud(), "viewer.no_files_loud", what_said());

  // 4. the player's files: the viewer publishes the table, and a wrestler material binds its row from it
  if (game.empty()) {
    std::printf("SKIP viewer.publishes / viewer.row_binds: no --game DIR\n");
  } else {
    launcher::GameFiles files;
    files.SetRoot(std::filesystem::u8path(game));
    said.clear();
    const bool pub = read_rule::PublishFromViewerFiles(&files, say);
    const auto t = read_rule::EffectTableNow();
    Check(pub && t && t->count("ych_skin"), "viewer.publishes",
          std::string(pub ? "published" : "not published") + (t ? ", " + std::to_string(t->size()) + " effects" : std::string(", no table")));
    if (t) {
      int bound_pass = -1;
      for (int p = 0; p < 12 && bound_pass < 0; ++p)
        if (ChooseCharEffectTable("yCh_skin", true, false, 0, p).Ok()) bound_pass = p;
      Check(bound_pass >= 0, "viewer.row_binds", bound_pass >= 0 ? "yCh_skin T0 pass " + std::to_string(bound_pass) + " binds its table row" : std::string("no pass of yCh_skin T0 binds"));
      // 5. S111 OLDCHAIN review (TECHPASS's +16 pass): an E+20 part (yCh_Aniso_trans) of a body whose model +620 is 1 is drawn in p17 (the hair class
      // carries no shadow bit), the game's ss_ row: the table pick binds that row (RowOf bounds the pass; no cap of ours turns it into a hole).
      const int p17 = technique_pick::CharPassOf(technique_pick::CharClassOf("ych_aniso_trans"), true, false, 0, false, 1);
      const CharChoice a17 = ChooseCharEffectTable("yCh_Aniso_trans", true, false, 0, p17);
      Check(p17 == 17 && a17.Ok() && a17.tech_picked && CharHoleOf(a17) == CharHole::None, "pass16.binds",
            "yCh_Aniso_trans T0 model+620 = 1: pass " + std::to_string(p17) + (a17.Ok() ? " binds its table row" : " is a hole: " + a17.why_not));
    }
  }

  if (g_plant) {
    std::printf("planted twins: %d caught, %d missed; other checks %s\n", g_caught, g_missed, g_bad ? "FAILED" : "ok");
    const bool good = g_caught > 0 && g_missed == 0 && g_bad == 0;
    std::printf("%s oldchain_unit --plant\n", good ? "CAUGHT:" : "NOT CAUGHT:");
    return good ? 0 : 1;
  }
  std::printf("%s oldchain_unit: no table = a counted hole + one loud line; the viewer publishes the table%s\n", g_bad ? "FAIL" : "PASS", game.empty() ? " (not run: no --game)" : "");
  return g_bad ? 1 : 0;
}
