// S111 BODYSPEC unit: the specular names a material reads, and its missing-power default, follow the game's name table of its effect family (src\assets\material_floats.h BeginRecord /
// ApplyFloat, the ONE table the file loader and the created-superstar build both use). 0 game runs (docs\research\s111\BODYSPEC.md).
//   out\tools\ychspec_unit.exe                   selftest: per family, through the production table, the stock and the guest (lower-case effect, the file's parameter names: both exact, S111 UNIFY3) names; the guest row consumer
//                                                (guestfloats::ApplyRows); both builders start their Material with BeginRecord before the parameters; the planted twin (the old rule: every
//                                                name for every family, power 300) must give the wrong answer on the yCh cases
//   out\tools\ychspec_unit.exe real --game DIR   read-only, the player's files: ch161 attire 0 through the production loader (LoadWrestler -> LoadJboy): its skin_flex parts keep their file
//                                                lobes (the census: power 14-30, level > 0) and every yCh part is marked as the yCh family
// Build + run: pwsh -NoProfile -File tools\scene\build_ychspec_unit.ps1
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/assets/asset_load.h"
#include "../../src/assets/material_floats.h"
#include "../../src/renderer_scene/guest_model_float_rows.h"

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

struct Lobe {
  float lev, pow, col;   // spec_lev, spec_pow, spec_col[0]
};
struct Param {
  std::string name;
  uint32_t kind;   // 0x0A scalar, 0x0D vec4
  float v;
};
using Params = std::vector<Param>;

// The production rule: BeginRecord, then every parameter through ApplyFloat, in record order (jboy.h LoadJboy; guest_model_draw.h BuildModel -> guestfloats::ApplyRows).
Lobe Production(const std::string& effect, const Params& ps) {
  assets::Material mat;
  assets::matfloat::BeginRecord(mat, effect);
  for (const Param& p : ps) {
    const float v[4] = {p.v, p.v, p.v, p.v};
    assets::matfloat::ApplyFloat(mat, p.name, p.kind, v, nullptr);
  }
  return {mat.spec_lev, mat.spec_pow, mat.spec_col[0]};
}

// The planted twin: the loader before the lane (both names of each lobe value for every family, Material's 300 when no power is named).
Lobe OldRule(const std::string&, const Params& ps) {
  assets::Material mat;
  for (const Param& p : ps) {
    if (p.kind == 0x0D && (p.name == "g_f4SpecularCol" || p.name == "g_f4SpecCol")) mat.spec_col[0] = p.v;
    else if (p.kind == 0x0A && (p.name == "g_fSpecularLev" || p.name == "g_fSpecLev")) mat.spec_lev = p.v;
    else if (p.kind == 0x0A && (p.name == "g_iSpecularPow" || p.name == "g_fSpecPow")) mat.spec_pow = p.v;
  }
  return {mat.spec_lev, mat.spec_pow, mat.spec_col[0]};
}

std::string Fmt(Lobe l) { return "level " + std::to_string(l.lev) + " power " + std::to_string(l.pow) + " colour " + std::to_string(l.col); }
std::string Lower(std::string s) {
  for (char& c : s) c = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
  return s;
}

// Each case with the game's answer (BODYSPEC.md section 3; material_floats.h BeginRecord): a name the family's table lacks gets no write (level 0 = off, colour white = Material's),
// a yCh part with no power draws 1 (the bake's 0 -> 1), every other family keeps 300.
struct Case {
  const char* name;
  const char* effect;
  Params ps;
  Lobe game;
  bool twin;   // the old rule gives another answer here
};
const std::vector<Case>& Cases() {
  static const std::vector<Case> c = {
      {"ych.legacy_names_no_lobe", "yCh_skin_flex", {{"g_fSpecularLev", 10, 5.f}, {"g_iSpecularPow", 10, 50.f}, {"g_f4SpecularCol", 13, 0.5f}}, {0.f, 1.f, 1.f}, true},
      {"ych.level_without_power_bakes_1", "yCh_skin_flex", {{"g_fSpecLev", 10, 2.f}}, {2.f, 1.f, 1.f}, true},
      {"ych.hair_names_no_power", "yCh_Aniso_trans", {{"g_SpecExp1", 10, 150.f}}, {0.f, 1.f, 1.f}, true},
      {"ych.file_values_kept", "yCh_skin", {{"g_fSpecLev", 10, 5.f}, {"g_fSpecPow", 10, 100.f}, {"g_f4SpecCol", 13, 0.5f}}, {5.f, 100.f, 0.5f}, false},
      {"ych.legacy_beside_new_ignored", "yCh_gear", {{"g_fSpecLev", 10, 1.5f}, {"g_fSpecularLev", 10, 9.f}, {"g_fSpecPow", 10, 20.f}, {"g_iSpecularPow", 10, 300.f}}, {1.5f, 20.f, 1.f}, true},
      {"ybg.legacy_names_read", "yBG_Default", {{"g_fSpecularLev", 10, 0.5f}, {"g_iSpecularPow", 10, 40.f}, {"g_f4SpecularCol", 13, 0.25f}}, {0.5f, 40.f, 0.25f}, false},
      {"ybg.ych_names_not_read", "yBG_Default", {{"g_fSpecLev", 10, 0.5f}, {"g_fSpecPow", 10, 40.f}, {"g_f4SpecCol", 13, 0.25f}}, {0.f, 300.f, 1.f}, true},
      {"yprop.no_power_keeps_300", "yProp_Default", {{"g_fSpecularLev", 10, 0.5f}}, {0.5f, 300.f, 1.f}, false},
      {"ydefault.no_power_keeps_300", "yDefault", {{"g_fSpecularLev", 10, 0.7f}, {"g_f4SpecularCol", 13, 0.25f}}, {0.7f, 300.f, 0.25f}, false},
  };
  return c;
}

bool Same(Lobe a, Lobe b) { return a.lev == b.lev && a.pow == b.pow && a.col == b.col; }

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// `first`, then `then` (once in the file), then `before`, in that order in `text` (the call sits after the line that sets the shader and before the parameters).
bool Order(const std::string& text, const std::string& first, const std::string& then, const std::string& before) {
  const size_t a = text.find(first), b = text.find(then), c = text.find(before);
  return a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c && text.find(then, b + 1) == std::string::npos;
}

int Selftest() {
  Check(assets::matfloat::YchTable("yCh_skin_flex") && assets::matfloat::YchTable("ych_aniso_trans") && assets::matfloat::YchTable("YCH_GEAR") && !assets::matfloat::YchTable("yBG_Default") &&
            !assets::matfloat::YchTable("yProp_Default") && !assets::matfloat::YchTable("yDefault") && !assets::matfloat::YchTable(""),
        "family.ych_by_effect_prefix", "the game's strncmp(lower-cased effect name, \"ych\", 3)");
  for (const Case& k : Cases()) {
    const Lobe got = Production(k.effect, k.ps);
    Check(Same(got, k.game), (std::string("stock.") + k.name).c_str(), Fmt(got) + " (game " + Fmt(k.game) + ")");
    // The guest: the game's lower-case effect name, the file's parameter names (the model build copies them as is), compared exactly (S111 UNIFY3: sub_82BAAEE8 0x82BADF6C).
    const Lobe gg = Production(Lower(k.effect), k.ps);
    Check(Same(gg, k.game), (std::string("guest.") + k.name).c_str(), Fmt(gg));
  }

  // The guest row consumer: the rows of a part, put on a Material that BuildModel started with BeginRecord.
  rdr_scene::GuestModelTables g;
  const auto row = [&](uint32_t part, const char* name, uint16_t kind, float v) {
    rdr_scene::GuestModelParam r{};
    r.model = 7, r.part = uint16_t(part), r.kind = kind;
    std::strncpy(r.name, name, sizeof r.name);
    for (float& x : r.v) x = v;
    g.params.push_back(r);
  };
  row(0, "g_fSpecularLev", 10, 5.f), row(0, "g_iSpecularPow", 10, 50.f);   // part 0: a yCh part naming only the old names
  row(1, "g_fSpecularLev", 10, 0.5f);                                        // part 1: a yDefault part naming a level and no power
  assets::Material m0, m1;
  assets::matfloat::BeginRecord(m0, "ych_skin"), assets::matfloat::BeginRecord(m1, "ydefault");
  const std::string k0 = rdr_scene::guestfloats::ApplyRows(g, 7, 0, m0), k1 = rdr_scene::guestfloats::ApplyRows(g, 7, 1, m1);
  Check(m0.spec_lev == 0.f && m0.spec_pow == 1.f && k0.empty() && m1.spec_lev == 0.5f && m1.spec_pow == 300.f && !k1.empty(), "guest.rows_by_family",
        "ych part level " + std::to_string(m0.spec_lev) + " power " + std::to_string(m0.spec_pow) + " key '" + k0 + "'; ydefault part level " + std::to_string(m1.spec_lev) + " power " +
            std::to_string(m1.spec_pow));
  // The row writer keeps a name any family's table takes (the consumer then applies the part's own family).
  Check(assets::matfloat::IsFloatParam("g_fSpecLev", 10) && assets::matfloat::IsFloatParam("g_fSpecularLev", 10) && assets::matfloat::IsFloatParam("g_f4SpecCol", 13) &&
            assets::matfloat::IsFloatParam("g_f4SpecularCol", 13) && assets::matfloat::IsFloatParam("g_iSpecularPow", 10) && !assets::matfloat::IsFloatParam("g_fNotAName", 10) &&
            !assets::matfloat::IsFloatParam("g_fspeclev", 10),
        "writer.keeps_both_families_names");

  // Both builders start the Material with BeginRecord between the shader name and the parameters (the source, beside this unit in the tree).
  const std::filesystem::path src = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "src";
  const std::string jboy = ReadText(src / "assets" / "jboy.h"), guest = ReadText(src / "renderer_scene" / "guest_model_draw.h");
  Check(Order(jboy, "matrec::BeginMaterial(mat, CString(r.b + rec + 0x7C, 16), r.u32(rec + 0x90), r.u32(rec + 0x8C));", "matsamp::ApplySamplerFacts(mat, pname, ti, in_table, tname, sctx);",
              "matfloat::ApplyFloat(mat, pname, kind, v, &m.warnings);"),   // S111 UNIFY5: the header step (BeginMaterial runs BeginRecord) comes before the parameter walk
        "loader.begins_record", jboy.empty() ? "jboy.h not found" : "");
  Check(Order(guest, "assets::matrec::BeginMaterial(mat, fx,", "const std::string shader = mat.shader;",
              "guestfloats::ApplyRows(t.guest_models, model, p, mat)"), "guest.begins_record",
        guest.empty() ? "guest_model_draw.h not found" : "");

  // ---- the planted twin: the old rule gives the cases it names lobes the game never draws
  int caught = 0, named = 0;
  for (const Case& k : Cases()) {
    if (!k.twin) continue;
    ++named;
    const Lobe o = OldRule(k.effect, k.ps);
    caught += Same(o, k.game) ? 0 : 1;
    std::printf("      twin %s: %s\n", k.name, Fmt(o).c_str());
  }
  Check(caught == named && named == 5, "twin.old_rule_caught", std::to_string(caught) + " of " + std::to_string(named) + " cases differ from the game under the old rule");

  std::printf("%s ychspec_unit selftest: the specular names and the missing power follow the family's name table\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

// The player's files, read-only: ch161 attire 0's skin_flex parts keep their file lobes (every stock yCh material names all four, BODYSPEC.md section 3), and the loader marks the family.
int Real(const char* game) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(game));
  assets::Model m;
  std::string err;
  const bool ok = assets::LoadWrestler(files, 161, 0, m, err);
  Check(ok && !m.materials.empty(), "real.load_ch161", ok ? std::to_string(m.materials.size()) + " materials" : err);
  int ych = 0, ych_marked = 0, flex = 0, flex_out = 0;
  for (const assets::Material& mt : m.materials) {
    if (!assets::matfloat::YchTable(mt.shader)) continue;
    ++ych, ych_marked += mt.ych_table ? 1 : 0;
    if (mt.shader != "yCh_skin_flex") continue;
    ++flex;
    flex_out += (mt.spec_pow < 14.f || mt.spec_pow > 30.f || mt.spec_lev <= 0.f) ? 1 : 0;
  }
  Check(ych > 0 && ych_marked == ych && flex > 0 && flex_out == 0, "real.flex_body_lobes_from_file",
        std::to_string(ych) + " yCh materials (" + std::to_string(ych_marked) + " marked yCh), " + std::to_string(flex) + " skin_flex, " + std::to_string(flex_out) +
            " outside the file census (power 14-30, level > 0)");
  std::printf("%s ychspec_unit real\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "real") {
    const char* game = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
      if (std::string(argv[i]) == "--game") game = argv[i + 1];
    if (!game) {
      std::printf("usage: ychspec_unit real --game DIR\n");
      return 2;
    }
    return Real(game);
  }
  return Selftest();
}
