// body-scale: none (a viewer of records: a Live wrestler with no known scale is a hole, as live)  [S103 RGF: body_scale_feed_lint.py]
// rsc_draw: loads a scene record from a file and draws it through the scene front-end into a PNG, with no viewer state
// (Session 101, lane 14). S103 lane R: the record -> frame work is the shared library (src\renderer_scene\record_frame.h, the class appliers rf_*.cpp); assets are
// loaded by pac + entry from the player's own files with the viewer's loader seam (offline_assets.h).
//   rsc_draw FILE.rsc --game DIR --out PICTURE.png [--size 640x360]
// Exit 0 when the picture is written; 2 on a bad record (unreadable, no camera, a frame the library cannot build); 3 on any other failure: an asset the player's files
// cannot give ("load failed"), a renderer / draw / write failure (the reason on stderr). An asset with no name in the record (a contributor placeholder) is skipped and said.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "offline_assets.h"
#include "png_out.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_c.h"
#include "../../src/assets/load_plants.h"

using namespace rdr_scene;

// S101 lane 26: `path` names a folder: it becomes <folder>/scene/frame_NNNNNN.rsc (or <folder>/frame_NNNNNN.rsc when it is the scene folder itself).
static bool ResolveRecord(std::string& path, long frame, std::string& err) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) return true;
  if (frame < 0) { err = "a folder needs --frame N"; return false; }
  char name[32];
  std::snprintf(name, sizeof(name), "frame_%06ld.rsc", frame);
  std::filesystem::path p = std::filesystem::path(path) / "scene" / name;
  if (!std::filesystem::exists(p, ec)) p = std::filesystem::path(path) / name;
  if (!std::filesystem::exists(p, ec)) { err = "no scene record for frame " + std::to_string(frame) + " under " + path + " (taken without --wwe2k14_scene_records?)"; return false; }
  path = p.string();
  return true;
}

int main(int argc, char** argv) {
  std::string rec, game, out;
  uint32_t w = 640, h = 360;
  int keep_kind = 0;
  int drop_kind = 0;   // S103 GRF: --drop-src-kind K removes the meshes of Mesh::src_kind K from every model (3 = the crowd baked into a whole-arena model)
  long frame = -1;  // S101 lane 26: FILE may be a capture folder (or its scene folder): --frame N picks frame_NNNNNN.rsc
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
    else if (a == "--drop-src-kind" && i + 1 < argc) drop_kind = std::atoi(argv[++i]);
    else if (a == "--keep-src-kind" && i + 1 < argc) keep_kind = std::atoi(argv[++i]);  // S101 lane27: keep only meshes of Mesh::src_kind K (3 = the crowd)
    else if (rec.empty()) rec = a;
  }
  if (rec.empty() || game.empty() || out.empty()) {
    std::fprintf(stderr, "usage: rsc_draw FILE.rsc|CAPTURE_FOLDER --frame N --game DIR --out PICTURE.png [--size 640x360]\n");
    return 1;
  }
  SceneTables t;
  std::string err;
  if (!ResolveRecord(rec, frame, err)) { std::fprintf(stderr, "rsc_draw: FAIL %s\n", err.c_str()); return 2; }
  if (!ReadRecord(rec, t, err)) { std::fprintf(stderr, "rsc_draw: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (!t.has_camera) { std::fprintf(stderr, "rsc_draw: FAIL the record has no camera\n"); return 2; }
  ViewerInit(game);
  assets::crowd_body::Init(game);  // S101 lane27

  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "rsc_draw: FAIL renderer: %s\n", err.c_str()); return 3; }
  FrontEndOptions fo;
  if (const char* v = std::getenv("WWE_RDR_NO_TEXTURES")) fo.textures = std::atoi(v) == 0;
  if (std::getenv("WWE_RDR_PLANT_NO_SCREEN_RULE")) rf::c::PlantsRef().no_screen_rule = true;   // S103 SCR planted twin
  if (std::getenv("WWE_RDR_NO_ARENA_FOLLOW")) rf::c::PlantsRef().no_arena_follow = true;  // S103 RLF: the planted twins of lane RL (the attribution runs of c3_arena); the game exe has no such read
  if (std::getenv("WWE_RDR_PROPS_SET100")) assets::load_detail::PropsSet100Plant() = true;
  int rc = 0;
  {
    FrontEnd fe(*renderer, fo);
    rf::c::SetOfflineLoad(true);  // S103 R2: lane C's arena applier loads offline only when told (live it queues through the drawer's loader)
    offline::Assets source(keep_kind, drop_kind);
    rf::ModelCache cache(fe, source, true);
    SceneFrame f;
    std::vector<rf::ClassReport> reports;  // (a Live record is converted to viewer space inside BuildFrame: the one space rule)
    if (!rf::BuildFrame(t, cache, rf::kAllFamilies, w, h, f, reports, err)) { std::fprintf(stderr, "rsc_draw: FAIL %s\n", err.c_str()); rc = 2; }
    else {
      std::vector<uint8_t> rgba;
      for (const rf::ClassReport& r : reports) {
        if (r.skipped) std::fprintf(stderr, "[rsc_draw] %s: %u of %u objects skipped: %s\n", rf::FamilyName(r.family), r.skipped, r.objects, r.why.c_str());
        if (r.why.find("load failed") != std::string::npos) { std::fprintf(stderr, "rsc_draw: FAIL asset: %s\n", r.why.c_str()); rc = 3; }  // (a record naming an asset the files cannot give is not drawn)
      }
      if (rc != 0) {}
      else if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "rsc_draw: FAIL draw: %s\n", err.c_str()); rc = 3; }
      else if (!scene_png::WritePngRgb(out, rgba, w, h)) { std::fprintf(stderr, "rsc_draw: FAIL cannot write %s\n", out.c_str()); rc = 3; }
      else {
        std::fprintf(stderr, "[rsc_draw] %zu objects drawn, validation errors: %llu\n%s", f.objects.size(), (unsigned long long)fe.validation_errors(), fe.CoverageText().c_str());
        std::printf("wrote %s (%ux%u)\n", out.c_str(), w, h);
      }
    }
    renderer->WaitIdle();
    cache.ReleaseRetired();
    for (int m : rf::c::TakeRetired()) fe.ReleaseModel(m);
  }
  return rc;
}
