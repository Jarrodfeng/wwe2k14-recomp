// S111 CHALPHA unit (docs\research\s111\CHALPHA.md): a wrestler colour draw is alpha-tested + blended (the game's category reset sub_82C22E88) only where its output alpha
// can fall below 1 (src\renderer_scene\char_alpha.h: a Texture pixel variant always, a Tint one on a draw whose MdlAlpha x MatAmbCol.w x MatDifCol.w constants are not all 1).
// 0 game runs. The class is the shader registry's (fx_registry.h Variant::out_alpha, compile_shaders.py CH_TINT_ALPHA); this unit checks every yCh_ pixel variant of the
// registry against the game's microcode class (the oC0.w trace in CHALPHA.md: the reference below), then the rules on each of those variants.
//   pwsh -NoProfile -File tools\scene\build_charalpha_unit.ps1
// --plant 1 = the ALPHAWREST rule (an alpha-1 Tint draw tested), --plant 2 = a Texture pair left untested: each must FAIL.
#include "../../src/renderer_scene/char_alpha.h"
#include "../../src/renderer/shaders/spv/fx_registry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_fail = 0;
int g_plant = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace ca = rdr_scene::char_alpha;

// front_end.cpp: BindMaterial's b->alpha_test OR Record's per-draw test (tint_discard), both from the bound pixel variant's out_alpha.
bool Tested(uint8_t oa, bool twin, float mdl, float amb, float dif) {
  return ca::BindTests(oa, twin, g_plant) || ca::DrawTests(oa, twin, mdl, amb, dif, g_plant);
}

// The reference: the game's microcode class of each yCh_ effect (CHALPHA.md, every hash traced), NOT the generator's list.
struct Ref { const char* effect; bool tint; };
const Ref kRef[] = {{"yCh_skin", true},         {"yCh_skin_flex", true},  {"yCh_gear", true},          {"yCh_gear_flex", true},   {"yCh_cloth", true},
                    {"yCh_cloth_flex", true},   {"yCh_Reflect", true},    {"yCh_Anisotropic", true},   {"yCh_trans", false},      {"yCh_TransTiling", false},
                    {"yCh_cloth_trans", false}, {"yCh_Aniso_trans", false}, {"yCh_Ls", false}};

// The registry ident is <effect>_<variant>_fragment: the longest reference effect that prefixes it (yCh_skin_flex before yCh_skin), or null.
const Ref* RefOf(const char* name) {
  const Ref* best = nullptr;
  for (const Ref& r : kRef) {
    const size_t n = std::strlen(r.effect);
    if (std::strncmp(name, r.effect, n) == 0 && name[n] == '_' && (!best || n > std::strlen(best->effect))) best = &r;
  }
  return best;
}

bool EndsWith(const char* s, const char* tail) {
  const size_t a = std::strlen(s), b = std::strlen(tail);
  return a >= b && std::strcmp(s + a - b, tail) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc) g_plant = std::atoi(argv[++i]);
  int n_tint = 0, n_tex = 0, n_twin_tint = 0, n_twin_tex = 0, wrong = 0, unknown = 0;
  bool tint_one = true, tint_low = true, tex = true, no_twin = true;
  for (const rdr::fx::Variant& v : rdr::fx::kVariants) {
    if (!v.name || std::strncmp(v.name, "yCh_", 4) != 0 || !EndsWith(v.name, "_fragment")) continue;
    const Ref* r = RefOf(v.name);
    if (!r) {
      if (++unknown <= 4) std::printf("      no reference class: %s\n", v.name);
      continue;
    }
    if ((v.out_alpha == ca::kTintAlpha) != r->tint) {
      if (++wrong <= 4) std::printf("      registry out_alpha %d, the game's class %s: %s\n", int(v.out_alpha), r->tint ? "Tint" : "Texture", v.name);
    }
    (r->tint ? n_tint : n_tex) += 1;
    const bool twin = v.bytes_alpha != 0;
    if (!twin) {
      no_twin = no_twin && !Tested(v.out_alpha, false, 0.5f, 1.f, 1.f);
      continue;
    }
    (r->tint ? n_twin_tint : n_twin_tex) += 1;
    if (r->tint) {
      tint_one = tint_one && !Tested(v.out_alpha, true, 1.f, 1.f, 1.f);
      tint_low = tint_low && Tested(v.out_alpha, true, 0.5f, 1.f, 1.f) && Tested(v.out_alpha, true, 1.f, 0.f, 1.f) && Tested(v.out_alpha, true, 1.f, 1.f, 0.99f);
    } else {
      tex = tex && Tested(v.out_alpha, true, 1.f, 1.f, 1.f) && Tested(v.out_alpha, true, 0.3f, 1.f, 1.f);
    }
  }
  char what[240];
  std::snprintf(what, sizeof what, "the registry's yCh_ pixel variants (%d Tint, %d Texture) carry the game's class (out_alpha 1 = Tint): %d differ, %d with no reference class", n_tint, n_tex, wrong, unknown);
  Check(n_tint > 0 && n_tex > 0 && !wrong && !unknown, what);
  std::snprintf(what, sizeof what, "every Tint pixel variant with a twin (%d) at MdlAlpha 1, MatAmbCol.w 1, MatDifCol.w 1: untested + unblended (output alpha 1)", n_twin_tint);
  Check(n_twin_tint > 0 && tint_one, what);
  Check(n_twin_tint > 0 && tint_low, "every Tint pixel variant with MdlAlpha 0.5, MatAmbCol.w 0, or MatDifCol.w 0.99: tested + blended, as the game");
  std::snprintf(what, sizeof what, "every Texture pixel variant with a twin (%d: the surface map's / diffuse / logo alpha) at constants 1 and below: tested + blended, as the game", n_twin_tex);
  Check(n_twin_tex > 0 && tex, what);
  Check(Tested(ca::kTintAlpha, true, std::nanf(""), 1.f, 1.f) && Tested(ca::kTintAlpha, true, 1.2f, 1.f, 1.f), "a NaN or an above-1 MdlAlpha: tested (not provably alpha 1)");
  Check(Tested(0, true, 1.f, 1.f, 1.f) && Tested(2, true, 1.f, 1.f, 1.f), "a pixel variant whose out_alpha is not the tint's (0 not provable, 2 a texture's): tested (the game's reset state)");
  Check(no_twin && !Tested(0, false, 1.f, 1.f, 1.f) && !Tested(ca::kTintAlpha, false, 0.5f, 1.f, 1.f), "no alpha twin: never tested (the program cannot discard)");
  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, g_plant == 1 ? " (planted twin 1: every pair tested)" : g_plant == 2 ? " (planted twin 2: a Texture pair untested)" : "");
  else std::printf("PASS  charalpha_unit\n");
  return g_fail ? 1 : 0;
}
