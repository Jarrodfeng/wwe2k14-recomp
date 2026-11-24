// S110 TRONSHEET review: the viewer's sheet picture (src\assets\mods_lookup.h ModSheet), host only, no game, no video decoder (a stub
// decoder hands back a 640x360 red frame). Built and run by tools\assets\mods_sheet_check.py.
// The case it guards: a custom tron slot (tron_slots_core::IsSlotId) is a sheet with NO original picture; with the default banner
// ("original") the strips must come out black (as the shim: "the original's strips could not be opened; black banners"), never read
// through the missing original (the pre-fix code dereferenced a null / empty original there).
#include "../../src/assets/mods_lookup.h"
#include "../../src/tron_sheet_core.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace ml = assets::mods_lookup;
namespace fs = std::filesystem;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& what) {
  std::printf("%s %s: %s\n", ok ? "ok" : "FAIL", name, what.c_str());
  if (!ok) ++g_fail;
}

static bool Px(const ml::Frame& f, int x, int y, int r, int g, int b) {
  const size_t i = (size_t(y) * f.w + x) * 4;
  return f.rgba[i] == r && f.rgba[i + 1] == g && f.rgba[i + 2] == b && f.rgba[i + 3] == 255;
}
// every pixel of rows [y0, y1) is (r, g, b)
static bool Rows(const ml::Frame& f, int y0, int y1, int r, int g, int b) {
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < f.w; ++x)
      if (!Px(f, x, y, r, g, b)) return false;
  return true;
}
static std::string Size(const ml::Frame& f) { return std::to_string(f.w) + "x" + std::to_string(f.h); }

int main() {
  const fs::path dir = fs::temp_directory_path() / "wwe2k14_mods_sheet_unit" / "mods";
  std::error_code ec;
  fs::remove_all(dir.parent_path(), ec);
  fs::create_directories(dir / "titantron", ec);
  for (const char* n : {"845.mp4", "107.mp4", "844.mp4"}) std::ofstream(dir / "titantron" / n, std::ios::binary) << "not a video";
  ml::Plat().decode = [](const fs::path&, double, ml::Frame& out, std::string&) {
    out.w = 640, out.h = 360;
    out.rgba.assign(size_t(out.w) * out.h * 4, 0);
    for (size_t i = 0; i < out.rgba.size(); i += 4) out.rgba[i] = 200, out.rgba[i + 3] = 255;
    return true;
  };
  const std::string why = ml::SetDir(dir);
  ml::Cfg().async = false;
  ml::Reindex();
  if (!why.empty() || !ml::Cfg().on) {
    std::printf("FAIL setup: the mods folder was refused (%s)\n", why.c_str());
    return 1;
  }
  const int S = int(wwe2k14::tron_sheet::kSheetSize), kBig = int(wwe2k14::tron_sheet::kSheetBigRows);

  // 1. a custom slot, no original at all (the viewer's mods_sheet command passes null), default banner = original: black strips
  {
    const ml::Frame f = ml::ModSheet(845, nullptr);
    const bool ok = f.ok() && f.w == S && f.h == S && Rows(f, 0, kBig, 200, 0, 0) && Rows(f, kBig, S, 0, 0, 0);
    Check(ok, "slot.no_original_null", "845 with a null original -> " + Size(f) + (ok ? ": the video on rows 0-223, black strips" : ": WRONG"));
  }
  // 2. the same with an EMPTY original (the stage / the tron layer pass the cache PNG, which a slot never has)
  {
    const ml::Frame none;
    const ml::Frame f = ml::ModSheet(845, &none);
    const bool ok = f.ok() && f.w == S && f.h == S && Rows(f, 0, kBig, 200, 0, 0) && Rows(f, kBig, S, 0, 0, 0);
    Check(ok, "slot.no_original_empty", "845 with an empty original -> " + Size(f) + (ok ? ": the video on rows 0-223, black strips" : ": WRONG"));
  }
  // 3. a stock number with a 320x320 original: the original's strips are kept
  {
    ml::Frame orig;
    orig.w = orig.h = S;
    orig.rgba.assign(size_t(S) * S * 4, 0);
    for (size_t i = 0; i < orig.rgba.size(); i += 4) orig.rgba[i + 2] = 180, orig.rgba[i + 3] = 255;
    const ml::Frame f = ml::ModSheet(107, &orig);
    const bool ok = f.ok() && f.w == S && f.h == S && Rows(f, 0, kBig, 200, 0, 0) && Rows(f, kBig, S, 0, 0, 180);
    Check(ok, "stock.original_strips", "107 with a 320x320 original -> " + Size(f) + (ok ? ": the original's strips" : ": WRONG"));
  }
  // 4. a stock number with no sheet original, and a number just outside the band: not a sheet (the video fills the frame)
  {
    const ml::Frame a = ml::ModSheet(107, nullptr), b = ml::ModSheet(844, nullptr);
    const bool ok = a.ok() && b.ok() && a.h != S && b.h != S;
    Check(ok, "not_slot.no_sheet", "107 / 844 without a sheet original -> " + Size(a) + " / " + Size(b) + (ok ? ": no sheet" : ": WRONG (a sheet)"));
  }
  fs::remove_all(dir.parent_path(), ec);
  std::printf("RESULT %s\n", g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
