// S111 SPECCOL unit: PS c144 (our SpecCubPrm3) = the material's g_f4SpecCol as a whole vec4 [C: sub_82BAAEE8 0x82BAC634..0x82BAC668, seam specular
// row 0x82BAC640], re-fed on every draw whose material changes (draw_lists.cpp kStMaterial). 0 game runs.
//   out\tools\speccol_unit.exe   -> PASS / FAIL, exit 0 / 1 (tools\scene\build_speccol_unit.ps1 builds and runs it)
// The two planted twins run inside and must FAIL: the old written-once class (mask 0) and the old resolver (a fixed (1, 1, 1, 0)).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/draw_lists.h"

using namespace rdr_scene;

namespace {
// One kept fragment block holding only SpecCubPrm3, drawn with the materials in `order` through the real Diff / Refresh (the material stamp h[5]
// differs per material, as front_end.cpp stamps it). `old_white`: the twin resolver. Returns the draws whose c144 is not their material's spec_col.
int Run(const assets::Material* const* order, int n, uint32_t mask, bool old_white, const char* label, bool verbose) {
  assets::LightSet ls;
  DrawLists L(1);
  KeptDraw d;
  d.fs.Begin(16);
  d.fs.fields.push_back(KeptField{"SpecCubPrm3", 0, 1, mask});
  int bad = 0;
  for (int i = 0; i < n; ++i) {
    const assets::Material* m = order[i];
    Stamps st;
    st.h[5] = uint64_t(reinterpret_cast<uintptr_t>(m));
    const uint32_t changed = DrawLists::Diff(d, st, i == 0);
    L.Refresh(d.fs, changed, [&](const KeptField& f, float* out) {
      if (old_white) {
        out[0] = out[1] = out[2] = 1.f, out[3] = 0.f;
        return;
      }
      CharCtx c;
      c.mat = m, c.lights = &ls;
      if (!ResolveChar(f.name, f.vec4s, c, out)) std::printf("FAIL  %s: ResolveChar does not know %s\n", label, f.name), ++bad;
    });
    float got[4];
    std::memcpy(got, d.fs.bytes.data(), 16);
    if (std::memcmp(got, m->spec_col, 16) != 0) {
      if (verbose)
        std::printf("FAIL  %s: draw %d c144 = (%g, %g, %g, %g), its material's g_f4SpecCol = (%g, %g, %g, %g)\n", label, i, got[0], got[1], got[2], got[3], m->spec_col[0],
                    m->spec_col[1], m->spec_col[2], m->spec_col[3]);
      ++bad;
    }
  }
  return bad;
}
}  // namespace

int main() {
  assets::Material ref, white, none;   // `none` names no g_f4SpecCol: the Material default is what it must get
  const float r[4] = {0.227f, 0.227f, 0.227f, 1.f};   // the referee ch255 gear (q asset mat)
  const float w[4] = {1.f, 1.f, 1.f, 1.f};
  std::memcpy(ref.spec_col, r, 16);
  std::memcpy(white.spec_col, w, 16);
  const assets::Material* order[] = {&ref, &white, &ref, &none, &ref};
  const int n = int(sizeof order / sizeof order[0]);
  int bad = 0;
  const uint32_t mask = FieldMask("SpecCubPrm3");
  if (mask != uint32_t(kStMaterial)) std::printf("FAIL  draw_lists.cpp: SpecCubPrm3 must be kStMaterial (re-fed per material), is %u\n", mask), ++bad;
  if (std::memcmp(none.spec_col, w, 16) != 0) std::printf("FAIL  Material::spec_col must default to white (1, 1, 1, 1)\n"), ++bad;
  const int real = Run(order, n, mask, false, "real", true);
  bad += real;
  const int twin_class = Run(order, n, 0u, false, "twin class 0", false);
  const int twin_white = Run(order, n, mask, true, "twin fixed white", false);
  std::printf("%s  the real path: %d of %d draws wrong\n", real ? "FAIL" : "ok  ", real, n);
  std::printf("%s  planted twin (SpecCubPrm3 written once, mask 0): %d stale draws %s\n", twin_class ? "ok  " : "FAIL", twin_class, twin_class ? "(caught)" : "(NOT caught)");
  std::printf("%s  planted twin (the old fixed (1, 1, 1, 0)): %d wrong draws %s\n", twin_white ? "ok  " : "FAIL", twin_white, twin_white ? "(caught)" : "(NOT caught)");
  if (!twin_class) ++bad;
  if (!twin_white) ++bad;
  std::printf("%s speccol_unit: c144 = the material's g_f4SpecCol on every draw\n", bad ? "FAIL" : "PASS");
  return bad ? 1 : 0;
}
