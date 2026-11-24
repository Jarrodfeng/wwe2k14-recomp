// body-scale: none (the model is drawn at its bind pose with an identity palette; the scale does not touch the draw state this unit reads)  [S103 RGF: body_scale_feed_lint.py]
// S108 HAIR: the piece-cut draw state through the REAL FrontEnd::Record (docs\research\s108\HAIR_plan.md section 8 item 2). Needs the player's game files (read only, --game DIR) and a Vulkan device (Renderer::Create + small
// off-screen draws; no window, no game run). Loads a wrestler attire through the loader the game uses (ViewerLoad: Undertaker 103 attire 2 by default = piece 10000 hair, 4 yCh_Aniso_trans meshes, 58 spring nodes), adds it to a
// FrontEnd, draws it at the bind pose (identity palette) and reads (a) the front end's own census of the Record (hair_cut_census) and (b) the final per-draw state the front end dumps (WWE_RDR_FEATURE_DUMP "draw" + "state" lines):
//   arm game   (the fix): every draw of a piece-10000 mesh: alpha test on, func GEQUAL, ref 128, depth write 1, blended; census draws > 0 = cut, notwin 0, nodisc 0; the body's draws are the same as in arm old.
//   arm old    (planted twin g_plant_piece_cut_old = the draw before S108): the hair draws untested, depth write 0; census draws 0. The game-arm checks must FAIL on it.
//   arm notest (planted twin g_plant_piece_cut_no_test = a depth write without the discard): census nodisc > 0 (the hole the no-twin rule forbids).
//   arm notwin (review: g_plant_piece_cut_no_twin = every pair reads as having no alpha twin): the no-twin rule: every hair draw untested with NO depth write, census notwin == draws, and a
//              piece_cut_notwin draw_shortfall. Arm notwin_norule (no twin AND the rule skipped by the notest plant) is its planted twin: the notwin check must FAIL on it.
//   piece_cut_frame_unit.exe --game DIR [--wrestler 103 --attire 2]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/problem.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/feature_dump.h"
#include "../../src/renderer_scene/front_end.h"

using namespace rdr_scene;

namespace {

struct DrawRow { std::string part, shader; int at = -1, af = -1, ar = -1, dw = -1, ben = -1, pc = -1; };

// The "draw" / "state" line pairs written since byte `from` of the dump file (a state line follows its draw line).
std::vector<DrawRow> ReadRows(const std::string& path, std::streamoff from) {
  std::vector<DrawRow> rows;
  std::ifstream in(path, std::ios::binary);
  in.seekg(from);
  std::string line;
  DrawRow cur;
  bool have = false;
  const auto str = [](const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = l.find(k);
    if (p == std::string::npos) return std::string();
    const size_t e = l.find('"', p + k.size());
    return l.substr(p + k.size(), e - p - k.size());
  };
  const auto num = [](const std::string& l, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = l.find(k);
    return p == std::string::npos ? -1 : std::atoi(l.c_str() + p + k.size());
  };
  while (std::getline(in, line)) {
    if (line.find("\"name\":\"frame_begin\"") != std::string::npos) { rows.clear(), have = false; continue; }   // one block per Record (RenderToPixels' seed Record comes first): the LAST block is the Record read
    if (line.find("\"k\":\"draw\"") != std::string::npos) { cur = DrawRow{}; cur.part = str(line, "part"), cur.shader = str(line, "shader"); have = true; }
    else if (have && line.find("\"k\":\"state\"") != std::string::npos) {
      cur.at = num(line, "at"), cur.af = num(line, "af"), cur.ar = num(line, "ar"), cur.dw = num(line, "dw"), cur.ben = num(line, "ben"), cur.pc = num(line, "pc");
      rows.push_back(cur);
      have = false;
    }
  }
  return rows;
}

Mat4 Ident() { Mat4 m{}; m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.f; return m; }

}  // namespace

int main(int argc, char** argv) {
  std::string game;
  int wrestler = 103, attire = 2;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
    else if (!std::strcmp(argv[i], "--wrestler") && i + 1 < argc) wrestler = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--attire") && i + 1 < argc) attire = std::atoi(argv[++i]);
  }
  if (game.empty()) { std::printf("usage: piece_cut_frame_unit --game DIR [--wrestler N --attire A]\n"); return 2; }
  if (const char* mc = std::getenv("RDR_MAIN_CHECKOUT"); mc && !std::filesystem::exists(game)) game = (std::filesystem::path(mc) / game).string();
  const char* dump = std::getenv("WWE_RDR_FEATURE_DUMP");
  if (!dump || !*dump) { std::printf("FAIL set WWE_RDR_FEATURE_DUMP=<scratch file> (the per-draw state lines this unit reads; build_piece_cut_unit.ps1 -Frame sets it)\n"); return 2; }
  ViewerInit(game);
  assets::Model model;
  std::string err;
  if (!ViewerLoad("wrestler", wrestler, attire, model, err)) { std::printf("FAIL load wrestler %d attire %d: %s\n", wrestler, attire, err.c_str()); return 2; }
  size_t hair_meshes = 0;
  for (const assets::Mesh& me : model.meshes) hair_meshes += (me.src_kind == 2 && me.src_key == 10000) ? 1u : 0u;
  std::printf("loaded %s: %zu meshes (%zu of piece 10000), %zu bones (%u body)\n", model.label.c_str(), model.meshes.size(), hair_meshes, model.bones.size(), model.main_bones);
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 2; }
  int bad = 0;
  auto check = [&](bool ok, const std::string& what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++bad; };
  struct Arm { FrontEnd::HairCutCensus census; std::vector<DrawRow> rows; uint64_t shortfalls = 0; };
  std::map<std::string, Arm> arms;
  {
    FrontEnd fe(*renderer);
    std::vector<std::string> warnings;
    const int id = fe.AddModel(model, warnings);
    if (id < 0) { std::printf("FAIL AddModel\n"); return 2; }
    for (const char* arm : {"game", "old", "notest", "notwin", "notwin_norule"}) {
      g_plant_piece_cut_old.store(!std::strcmp(arm, "old"));
      g_plant_piece_cut_no_test.store(!std::strcmp(arm, "notest") || !std::strcmp(arm, "notwin_norule"));
      g_plant_piece_cut_no_twin.store(!std::strcmp(arm, "notwin") || !std::strcmp(arm, "notwin_norule"));
      const uint64_t shortfalls0 = rdr::DrawShortfallTally().distinct;
      SceneFrame sf;
      sf.width = 320, sf.height = 180;
      sf.viewer_space = false;
      sf.camera.view = Ident();
      sf.camera.proj = Ident();
      sf.camera.view.m[0] = sf.camera.view.m[5] = 0.008f, sf.camera.view.m[10] = 0.004f, sf.camera.view.m[13] = -0.8f, sf.camera.view.m[14] = 0.5f;   // the ~200-unit model inside the unit square, depth inside 0..1
      Object ob;
      ob.asset = uint32_t(id);
      ob.world = Ident();
      ob.flags |= kObjSkinned;
      ob.bones_first = 0, ob.bones_count = uint32_t(model.bones.size());
      sf.bones.assign(model.bones.size(), Ident());   // the bind pose: every node at its bind (palette = pose x inverse bind = identity)
      sf.objects.push_back(ob);
      FILE* f = fdump::File();
      if (!f) { std::printf("FAIL the feature dump file could not be opened\n"); return 2; }
      std::fflush(f);
      const std::streamoff from = std::streamoff(std::filesystem::file_size(dump));
      std::vector<uint8_t> rgba;
      // one call: RenderToPixels records a seed frame (in_seed_: no census) then the frame read; the census is the last Record's, the dump's last block (ReadRows) the same Record
      if (!fe.RenderToPixels(sf, rgba, err)) { std::printf("FAIL draw (%s): %s\n", arm, err.c_str()); return 2; }
      std::fflush(f);
      arms[arm].census = fe.hair_cut_census();
      arms[arm].rows = ReadRows(dump, from);
      arms[arm].shortfalls = rdr::DrawShortfallTally().distinct - shortfalls0;
      const FrontEnd::HairCutCensus& c = arms[arm].census;
      std::printf("arm %-13s census: draws %u, cut %u, notwin %u, nodisc %u; dump rows %zu; new draw_shortfall keys %llu\n", arm, c.draws, c.cut, c.notwin, c.nodisc, arms[arm].rows.size(), (unsigned long long)arms[arm].shortfalls);
    }
    g_plant_piece_cut_old.store(false), g_plant_piece_cut_no_test.store(false), g_plant_piece_cut_no_twin.store(false);
    renderer->WaitIdle();
  }
  const auto is_hair = [](const DrawRow& r) { return r.part == "piece 10000"; };
  const auto hair_ok = [&](const Arm& a) {   // every hair draw in the game's state; at least one
    size_t n = 0;
    for (const DrawRow& r : a.rows) {
      if (!is_hair(r)) continue;
      ++n;
      if (!(r.at == 1 && r.af == 6 && r.ar == 128 && r.dw == 1 && r.ben == 1)) return false;
    }
    return n > 0;
  };
  const Arm& g = arms["game"];
  const Arm& o = arms["old"];
  const Arm& nt = arms["notest"];
  size_t hair_rows = 0;
  for (const DrawRow& r : g.rows) hair_rows += is_hair(r) ? 1u : 0u;
  check(hair_meshes > 0, "the attire has piece-10000 meshes (" + std::to_string(hair_meshes) + ")");
  check(hair_rows > 0, "arm game: the dump holds piece-10000 draws (" + std::to_string(hair_rows) + ")");
  check(hair_ok(g), "arm game: every piece-10000 draw is alpha-tested GEQUAL 128/255, blended, with depth write");
  check(g.census.draws > 0 && g.census.cut == g.census.draws && g.census.notwin == 0 && g.census.nodisc == 0, "arm game: census draws == cut > 0, notwin 0, nodisc 0");
  check(g.census.draws == hair_rows, "arm game: the census counts the dumped hair draws (" + std::to_string(g.census.draws) + " vs " + std::to_string(hair_rows) + ")");
  // the body is untouched: the same non-hair rows (part, shader, state) in both arms
  std::vector<std::string> gb, ob2;
  for (const DrawRow& r : g.rows) if (!is_hair(r)) gb.push_back(r.part + "|" + r.shader + "|" + std::to_string(r.at) + std::to_string(r.ar) + std::to_string(r.dw) + std::to_string(r.ben) + std::to_string(r.pc));
  for (const DrawRow& r : o.rows) if (!is_hair(r)) ob2.push_back(r.part + "|" + r.shader + "|" + std::to_string(r.at) + std::to_string(r.ar) + std::to_string(r.dw) + std::to_string(r.ben) + std::to_string(r.pc));
  check(!gb.empty() && gb == ob2, "the body's draws are identical with and without the rule (" + std::to_string(gb.size()) + " draws)");
  // planted twin old: the game-arm checks must fail on it
  bool old_hair_untested = true;
  size_t old_hair = 0;
  for (const DrawRow& r : o.rows) if (is_hair(r)) ++old_hair, old_hair_untested = old_hair_untested && r.at == 0 && r.dw == 0;
  check(old_hair > 0 && old_hair_untested && !hair_ok(o) && o.census.draws == 0, "planted twin old (the draw before S108): the hair is untested with no depth write, and the game-arm check FAILS on it");
  check(nt.census.nodisc > 0 && nt.census.cut == 0, "planted twin notest (a depth write without the discard): the census shows it (nodisc " + std::to_string(nt.census.nodisc) + ")");
  // review: the no-twin rule (never a depth write without a discard), driven through the real Record by the no-twin plant; its planted twin (the rule skipped) must fail the same check
  const auto notwin_ok = [&](const Arm& a) {
    size_t n = 0;
    for (const DrawRow& r : a.rows) {
      if (!is_hair(r)) continue;
      ++n;
      if (!(r.at == 0 && r.dw == 0 && r.ben == 1)) return false;
    }
    return n > 0 && a.census.draws == n && a.census.notwin == a.census.draws && a.census.cut == 0 && a.census.nodisc == 0;
  };
  const Arm& nw = arms["notwin"];
  const Arm& nwr = arms["notwin_norule"];
  check(notwin_ok(nw), "arm notwin (no alpha twin): every piece-10000 draw is blended, untested, with NO depth write; census notwin == draws, cut 0, nodisc 0");
  check(nw.shortfalls >= 1, "arm notwin: the piece_cut_notwin draw_shortfall is written (" + std::to_string(nw.shortfalls) + " new key(s))");
  check(!notwin_ok(nwr) && nwr.census.nodisc > 0, "planted twin notwin_norule (no twin, the rule skipped): the notwin check FAILS on it (nodisc " + std::to_string(nwr.census.nodisc) + ")");
  std::printf(bad ? "piece_cut_frame_unit: FAIL (%d)\n" : "piece_cut_frame_unit: PASS\n", bad);
  return bad ? 1 : 0;
}
