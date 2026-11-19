// eyes_frame_unit: lane EYF's proof through the REAL code (Session 103, review item 5; the record-only proof is eyes_unit.cpp). Needs the game files (read only, --game DIR) and a Vulkan device
// (Renderer::Create + one 320x180 draw; no game run).  Usage: eyes_frame_unit --record FRAME.rsc --game DIR [--plant one] [--expect-fail]
// It runs rf::BuildFrame (wrestlers) on the record, then FrontEnd::RenderToPixels, and checks:
//  1. every kObjDrawnByOwner piece of the record has a SceneFrame::piece_alphas row whose object is the matching "actor:slot:N" owner (N = the piece's Object::layer - 1), whose piece id is the piece's
//     Object::pad and whose alpha is the record's latched [model+252] (kScModelRuntime) for the piece's engine object;
//  2. the owner's model holds a mesh of each such piece (ModelHasPieceMesh);
//  3. the front end skipped meshes of alpha-0 rows (piece_mesh_stats().skipped_alpha0 > 0 when such a row has a mesh) and drew meshes of alpha > 0 rows (drawn > 0);
//  4. no piece is a missing-row hole (pieces_alpha_missing == 0).
// --plant one = the bug (every piece at alpha 1): check 1 fails for the lid layer; --expect-fail inverts the exit code for the gate row.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "offline_assets.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/body_scale.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/piece_alpha.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_wrestlers.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  std::string rec, game;
  bool plant = false, expect_fail = false, norows = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--record") && i + 1 < argc) rec = argv[++i];
    else if (!std::strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) { const char* p = argv[++i]; plant = !std::strcmp(p, "one"); norows = !std::strcmp(p, "norows"); }
    else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
  }
  if (rec.empty() || game.empty()) { std::printf("usage: eyes_frame_unit --record FRAME.rsc --game DIR [--plant one|norows] [--expect-fail]\n"); return 2; }
  if (const char* mc = std::getenv("RDR_MAIN_CHECKOUT")) {   // a gate row names game_data / the record under the MAIN checkout (a worktree has neither)
    auto fix = [&](std::string& p) { if (!std::filesystem::exists(p)) p = (std::filesystem::path(mc) / p).string(); };
    fix(rec), fix(game);
  }
  SceneTables t;
  std::string err;
  if (!ReadRecord(rec, t, err)) { std::printf("FAIL bad record: %s\n", err.c_str()); return 2; }
  for (uint32_t addr : t.engine_objects) BodyScaleTable::Get().Set(addr, 1.0f);   // a case from a live run has no body-scale table (frame_rig --body-scale-one, [U]): the scale does not touch the piece alphas
  ViewerInit(game);
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 2; }
  if (plant) rf::Plants().piece_alpha_one = true;
  if (norows) t.model_runtime.clear();   // review item 2: a record with no [model+252] rows: every linked piece must be a named HOLE (the frame falls back), never a silent hide
  int bad = 0;
  auto check = [&](bool ok, const std::string& what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++bad; };
  {
    FrontEnd fe(*renderer);
    offline::Assets source;
    rf::ModelCache cache(fe, source, true);
    SceneFrame f;
    std::vector<rf::ClassReport> reports;
    std::string e;
    cache.NewFrame();
    if (!rf::BuildFrame(t, cache, 1u << uint32_t(rf::Family::Wrestlers), 320, 180, f, reports, e)) { std::printf("FAIL BuildFrame: %s\n", e.c_str()); return 2; }
    std::printf("BuildFrame: %zu objects, %zu piece_alphas rows\n", f.objects.size(), f.piece_alphas.size());
    for (const rf::ClassReport& r : reports) std::printf("  %s: %u objects, %u drawn, %u skipped, holes %u: %s\n", rf::FamilyName(r.family), r.objects, r.drawn, r.skipped, r.holes, r.why.c_str());
    unsigned pieces = 0, with_mesh_zero = 0, with_mesh_pos = 0;
    if (norows) {   // review item 2: the missing-row twin: every linked piece is a named hole, never a silent hide
      for (const Object& o : t.objects) pieces += (o.flags & kObjDrawnByOwner) ? 1u : 0u;
      unsigned holes = 0;
      for (const rf::ClassReport& r : reports)
        if (r.family == rf::Family::Wrestlers) holes = r.holes;
      check(pieces > 0 && rf::LastWrestlerStats().pieces_alpha_missing == pieces && holes >= pieces,
            "no model_runtime rows: every piece (" + std::to_string(pieces) + ") is a missing-row hole (wrestlers holes " + std::to_string(holes) + ")");
      std::printf(bad ? "FAIL\n" : "PASS\n");
      return bad ? 1 : 0;
    }
    for (size_t i = 0; i < t.objects.size(); ++i) {
      const Object& o = t.objects[i];
      if (!(o.flags & kObjDrawnByOwner)) continue;
      ++pieces;
      const rf::PieceAlphaValue want = rf::PieceModelAlpha(t, i);   // the latched row, never the plant
      const rdr_scene::SceneFrame::PieceAlpha* got = nullptr;
      for (const SceneFrame::PieceAlpha& pa : f.piece_alphas)
        if (pa.piece == o.pad && pa.object < f.objects.size()) {
          const std::string_view oid = t.Str(f.objects[pa.object].id);
          if (oid == "actor:slot:" + std::to_string(int(o.layer) - 1)) got = &pa;
        }
      const std::string name = std::string(t.Str(o.id)) + " piece " + std::to_string(o.pad) + " slot " + std::to_string(int(o.layer) - 1);
      check(got != nullptr, name + ": a piece_alphas row on the owner actor:slot:N");
      if (!got) continue;
      check(want.found && got->alpha == want.alpha, name + ": carried alpha " + std::to_string(got->alpha) + " == the latched [model+252] " + std::to_string(want.alpha));
      const bool mesh = cache.front_end().ModelHasPieceMesh(int(f.objects[got->object].asset), o.pad);
      check(mesh, name + ": the owner's model holds a mesh of the piece");
      if (mesh) (want.alpha > 0.f ? with_mesh_pos : with_mesh_zero)++;
    }
    check(pieces > 0, "the record holds own-skeleton pieces (" + std::to_string(pieces) + ")");
    check(rf::LastWrestlerStats().pieces_alpha_missing == 0, "no piece is a missing-row hole");
    std::vector<uint8_t> rgba;
    if (!fe.RenderToPixels(f, rgba, err)) { std::printf("FAIL draw: %s\n", err.c_str()); return 2; }
    const FrontEnd::PieceMeshStats& s = fe.piece_mesh_stats();
    std::printf("front end: %u piece meshes skipped at alpha 0, %u drawn with their alpha, %u lid meshes with no row skipped (rows with a mesh: %u at 0, %u above 0)\n", s.skipped_alpha0, s.drawn, s.lid_no_row_skipped, with_mesh_zero, with_mesh_pos);
    check(with_mesh_zero == 0 || s.skipped_alpha0 >= with_mesh_zero, "meshes of alpha-0 pieces are skipped");
    check(with_mesh_pos == 0 || s.drawn >= with_mesh_pos, "meshes of alpha > 0 pieces are drawn");
    renderer->WaitIdle();
    cache.ReleaseRetired();
  }
  if (expect_fail) { std::printf(bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", bad); return bad ? 0 : 1; }
  std::printf(bad ? "FAIL: %d check(s)\n" : "PASS\n", bad);
  return bad ? 1 : 0;
}
