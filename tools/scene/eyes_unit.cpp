// eyes_unit: the offline proof of lane EYF (Session 103; docs\research\s103\EYES.md). Reads scene records and, for every own-skeleton piece the owner's body draws
// (kObjDrawnByOwner), prints the piece's id, piece id (Object::pad), owner slot (Object::layer - 1) and the kScModelRuntime row (MdlAlpha = [model+252]) of its engine object.
// Proof: the alpha the renderer carries (rf::PieceModelAlpha) equals the latched row for every piece that has one; a piece with no readable row is 0 (fail closed, never 1);
// and the planted twin (every piece at alpha 1, the bug) is CAUGHT: it differs from the latched row on at least one piece (the eyelid layer, id 20002, when the record holds one).
// Usage: eyes_unit FILE.rsc | CAPTURE_DIR --frame N [more files / --frame ...] [--quiet]     Exit 0 = proof holds on every record, 1 = a failure, 2 = a record did not read.
// Build: tools\scene\build_eyes_unit.ps1 (compile only: record_io + this file).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/renderer_scene/piece_alpha.h"
#include "../../src/renderer_scene/rows_inputs.h"

using namespace rdr_scene;

static bool LoadAny(const std::string& arg, long frame, SceneTables& t, std::string& err) {
  std::filesystem::path p = arg;
  std::error_code ec;
  if (p.is_relative() && !std::filesystem::exists(p, ec))   // a check row names the record under out\ of the MAIN checkout (records are not in a worktree)
    if (const char* mc = std::getenv("RDR_MAIN_CHECKOUT")) p = std::filesystem::path(mc) / p;
  if (std::filesystem::is_directory(p, ec)) {
    char name[32];
    std::snprintf(name, sizeof(name), "frame_%06ld.rsc", frame);
    p = p / "scene" / name;
  }
  return ReadRecord(p.string(), t, err);
}

int main(int argc, char** argv) {
  std::vector<std::pair<std::string, long>> inputs;
  bool quiet = false, plant = false, expect_fail = false;
  long frame = -1;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--quiet")) quiet = true;
    else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = !std::strcmp(argv[++i], "one");
    else if (!std::strcmp(argv[i], "--frame") && i + 1 < argc) frame = std::atol(argv[++i]), inputs.emplace_back(paths.empty() ? "" : paths.back(), frame);
    else paths.push_back(argv[i]);
  }
  if (inputs.empty())
    for (const std::string& p : paths) inputs.emplace_back(p, -1);
  if (inputs.empty()) {
    std::fprintf(stderr, "usage: eyes_unit FILE.rsc | CAPTURE_DIR --frame N ... [--quiet]\n");
    return 2;
  }
  int fails = 0, unreadable = 0;
  size_t total_pieces = 0, with_row = 0, lids = 0, lid_rows = 0, discriminating = 0, lid_discriminating = 0, rows_total = 0;
  for (const auto& in : inputs) {
    SceneTables t;
    std::string err;
    if (!LoadAny(in.first, in.second, t, err)) {
      std::fprintf(stderr, "%s frame %ld: does not read: %s\n", in.first.c_str(), in.second, err.c_str());
      ++unreadable;
      continue;
    }
    rows_total += t.model_runtime.size();
    size_t pieces = 0, rows = 0;
    for (size_t i = 0; i < t.objects.size(); ++i) {
      const Object& o = t.objects[i];
      if (!(o.flags & kObjDrawnByOwner)) continue;
      ++pieces;
      const uint32_t addr = i < t.engine_objects.size() ? t.engine_objects[i] : 0u;
      const rf::PieceAlphaValue v = rf::PieceModelAlpha(t, i, plant);   // --plant one: the carried value IS the bug (alpha 1); the proof below must FAIL (the check row expects it)
      const rf::PieceAlphaValue planted = rf::PieceModelAlpha(t, i, true);
      float row_alpha = -1.f;
      bool have = false;
      for (const ModelRuntimeRow& m : t.model_runtime)
        if (addr && m.object == addr && (m.flags & kMrAlphaRead)) have = true, row_alpha = m.alpha;   // last row wins, as the function does
      const bool is_lid = o.pad == 20002;
      if (have) ++rows;
      if (is_lid) ++lids, lid_rows += have;
      bool ok;
      if (have) {
        const float want = std::isnan(row_alpha) ? 0.f : (row_alpha < 0.f ? 0.f : (row_alpha > 1.f ? 1.f : row_alpha));
        ok = v.found && v.alpha == want;
        if (planted.alpha != want) ++discriminating, lid_discriminating += is_lid;
      } else {
        ok = !v.found && v.alpha == 0.f;   // fail closed
      }
      if (!ok) ++fails;
      if (!quiet || !ok)
        std::printf("%s f%ld obj %zu %s piece %u slot %d addr %08X row %s alpha %.4f carried %.4f%s%s\n", in.first.c_str(), in.second, i, std::string(t.Str(o.id)).c_str(), unsigned(o.pad), int(o.layer) - 1, addr,
                    have ? "yes" : "NONE", have ? row_alpha : 0.f, v.alpha, is_lid ? " [lid layer]" : "", ok ? "" : "  FAIL");
    }
    total_pieces += pieces, with_row += rows;
    std::printf("%s frame %ld: %zu objects, %zu model_runtime rows, %zu own-skeleton pieces drawn by owner, %zu with a row\n", in.first.c_str(), in.second, t.objects.size(), t.model_runtime.size(), pieces, rows);
  }
  std::printf("TOTAL: %zu records read (%d unreadable), %zu rows, %zu pieces, %zu with a row, lid layers (20002) %zu (with a row %zu); the planted alpha-1 twin differs on %zu piece(s), %zu of them lid layers\n",
              inputs.size() - size_t(unreadable), unreadable, rows_total, total_pieces, with_row, lids, lid_rows, discriminating, lid_discriminating);
  if (unreadable) return 2;
  if (!total_pieces) { std::printf("NO PIECES in these records: nothing proven\n"); return 1; }
  if (with_row && !discriminating) { std::printf("the planted twin (alpha 1 everywhere) is NOT caught: every latched row is already 1, this record cannot prove the fix\n"); return 1; }
  if (expect_fail) {   // the planted twin's row: the proof must fail on it
    std::printf(fails ? "planted twin CAUGHT (%d piece(s) differ from the latched row): PASS\n" : "planted twin NOT caught: FAIL\n", fails);
    return fails ? 0 : 1;
  }
  std::printf(fails ? "FAIL: %d piece(s)\n" : "PASS\n", fails);
  return fails ? 1 : 0;
}
