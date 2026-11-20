// S103 EYA unit: the eyeball's ambient scale. ResolveChar("Param1") = PS c247 = (g_fFurLevel, 0, g_fDifAmbLevel, 0) from the material
// [C: sub_82BAAEE8 0x82BAD37C id 3 -> x, 0x82BAD3B4 id 5 -> z]. 0 game runs.
//   out\tools\eya_unit.exe            -> PASS / FAIL, exit 0 / 1
//   out\tools\eya_unit.exe --plant    (EyaOldParam1Plant set: the old zero list) must FAIL inside, so the plant run prints PASS only when it was caught
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/assets/load_plants.h"
#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/read_rule.h"             // S105 A5c fix F1: the effect-pick test publishes fixture tables
#include "../../src/renderer/shaders/spv/fx_registry.h"

using namespace rdr_scene;

int main(int argc, char** argv) {
  const bool plant = argc > 1 && !std::strcmp(argv[1], "--plant");
  if (plant) assets::load_detail::EyaOldParam1Plant() = true;   // before the first resolve: the plant switch is read once
  assets::LightSet ls;
  const auto resolve = [&](float fur, float dif, float* out) {
    assets::Material mat;
    mat.fur_level = fur, mat.dif_amb_level = dif;
    CharCtx c;
    c.mat = &mat, c.lights = &ls;
    return ResolveChar("Param1", 1, c, out);
  };
  float a[4] = {9, 9, 9, 9}, b[4] = {9, 9, 9, 9}, d[4] = {9, 9, 9, 9};
  const bool ka = resolve(0.25f, 0.8f, a), kb = resolve(0.f, 0.3f, b);
  assets::Material dflt;
  CharCtx c;
  c.mat = &dflt, c.lights = &ls;
  ResolveChar("Param1", 1, c, d);
  int bad = 0;
  if (!ka || !kb) std::printf("FAIL  Param1 is not a known name\n"), ++bad;
  if (a[0] != 0.25f || a[2] != 0.8f) std::printf("FAIL  g_fFurLevel 0.25 / g_fDifAmbLevel 0.8 gave x %g z %g\n", a[0], a[2]), ++bad;
  if (b[2] != 0.3f || a[2] == b[2]) std::printf("FAIL  a material with another g_fDifAmbLevel does not change c247.z (%g vs %g)\n", a[2], b[2]), ++bad;
  if (a[1] != 0.f || a[3] != 0.f) std::printf("FAIL  c247.y / .w must stay 0 [U]: %g %g\n", a[1], a[3]), ++bad;
  if (d[0] != 0.f || d[2] != 0.f) std::printf("FAIL  a material without the parameters must give 0 [L]\n"), ++bad;
  {   // S105 A5c (entry: RoleOfSlot / ResolveChar, the front end's own calls): the logo slots and the game's pixel bool 150
    if (RoleOfSlot(7) != CharSlot::Logo7 || RoleOfSlot(4) != CharSlot::Logo4 || RoleOfSlot(2) != CharSlot::FlatBlack) std::printf("FAIL  A5c: fetch slots 7 / 4 are the CAW logo roles (2 stays flat black)\n"), ++bad;
    float bo[8] = {}, bn[8] = {};
    assets::Material t0, no;
    t0.diffuse_t0 = true;
    CharCtx c0, c1;
    c0.mat = &t0, c0.lights = &ls, c1.mat = &no, c1.lights = &ls;
    ResolveChar("Bools", 2, c0, bo), ResolveChar("Bools", 2, c1, bn);
    uint32_t o4, n4, o0, n0;
    std::memcpy(&o4, bo + 4, 4), std::memcpy(&n4, bn + 4, 4), std::memcpy(&o0, bo, 4), std::memcpy(&n0, bn, 4);
    if (o4 != (1u << 22) || n4 != 0 || o0 != 0xEu || n0 != 0xEu) std::printf("FAIL  A5c: Bools bit 150 (word 4 bit 22) follows Material::diffuse_t0 and the bone bits stay (%08X %08X / %08X %08X)\n", o4, n4, o0, n0), ++bad;
  }
  {   // S105 A5c fix F1 (review_A5c): the effect pick end to end. A material with a CAW logo takes its row from the editor's table (edit.pac p7/0: every yCh fragment a logo variant); every other material keeps the startup row.
    // Fixture tables are built from the registry's own variants (no game file): a yCh_skin vertex V, a logo fragment L, a plain fragment S that the pick can bind.
    using rdr::fx::kVariants;
    constexpr int nv = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));
    const auto hash_of = [&](int v) { for (const auto& h : rdr::fx::kHashes) if (int(h.variant) == v) return uint64_t(h.hash); return uint64_t(0); };
    const auto is_own = [&](int v, const char* suffix) {   // yCh_skin_ (not yCh_skin_flex_) variant ending `suffix`
      const char* n = kVariants[v].name;
      const size_t len = n ? std::strlen(n) : 0, sl = std::strlen(suffix);
      return n && std::strncmp(n, "yCh_skin_", 9) == 0 && std::strncmp(n, "yCh_skin_flex_", 14) != 0 && len > sl && !std::strcmp(n + len - sl, suffix);
    };
    const auto has_logo = [&](int v) { return kVariants[v].name && std::strstr(kVariants[v].name, "logo") != nullptr; };
    const auto make = [&](uint64_t vs, uint64_t ps) {
      auto t = std::make_shared<read_rule::EffectTable>();
      read_rule::EffectTech et;
      et.name = "yCh_skin";
      et.techs.assign(1, std::vector<read_rule::PassShaders>(12, read_rule::PassShaders{vs, ps, true, true}));
      t->emplace("ych_skin", std::move(et));
      return std::shared_ptr<const read_rule::EffectTable>(t);
    };
    int fv = -1, fl = -1, fs = -1, fpass = 0;
    for (int pass : {0, 2}) {
      for (int v = 0; v < nv && fv < 0; ++v) {
        if (!is_own(v, "_vertex") || !hash_of(v)) continue;
        int l = -1, p = -1;
        for (int f = 0; f < nv && (l < 0 || p < 0); ++f) {
          if (!is_own(f, "_fragment") || !hash_of(f)) continue;
          read_rule::PublishEffectTable(make(hash_of(v), hash_of(f)));
          read_rule::PublishLogoEffectTable(nullptr);
          const CharChoice cc = ChooseCharEffectTable("yCh_skin", false, false, 0, pass, false);
          if (!cc.Ok() || cc.ps != f) continue;
          if (has_logo(f)) { if (l < 0) l = f; } else if (p < 0) p = f;
        }
        if (l >= 0 && p >= 0) fv = v, fl = l, fs = p, fpass = pass;
      }
      if (fv >= 0) break;
    }
    if (fv < 0) std::printf("FAIL  A5c F1: the registry has no bindable yCh_skin vertex + logo fragment + plain fragment to build the fixture from\n"), ++bad;
    else {
      read_rule::PublishEffectTable(make(hash_of(fv), hash_of(fs)));   // the startup table: the plain row
      read_rule::PublishLogoEffectTable(make(hash_of(fv), hash_of(fl)));   // the editor's table: the logo row
      const auto pick = [&](bool logo) { return ChooseCharEffectTable("yCh_skin", false, false, 0, fpass, logo); };
      // the verdict the front end relies on: the logo row for a logo material, the startup row for any other (a match model, a stock wrestler)
      const auto verdict = [&](const CharChoice& cc, bool want_logo) { return cc.Ok() && cc.logo_table == want_logo && has_logo(cc.ps) == want_logo && cc.ps == (want_logo ? fl : fs); };
      const bool logo_ok = verdict(pick(true), true), plain_ok = verdict(pick(false), false);
      if (!logo_ok) std::printf("FAIL  A5c F1: a logo material did not pick the editor table's logo row\n"), ++bad;
      if (!plain_ok) std::printf("FAIL  A5c F1: a material without a logo did not keep the startup row\n"), ++bad;
      // planted twins, in the unsafe direction: the startup row used for a logo material, the logo row used for a match model; the verdict must refuse both
      if (verdict(pick(false), true)) std::printf("FAIL  A5c F1 twin: the startup row for a logo material was accepted\n"), ++bad;
      if (verdict(pick(true), false)) std::printf("FAIL  A5c F1 twin: the logo row for a plain material was accepted\n"), ++bad;
      read_rule::PublishLogoEffectTable(nullptr);   // no editor table (not built / failed): the startup row stays, nothing is drawn for the logo (as before), never a crash
      if (!verdict(pick(true), false)) std::printf("FAIL  A5c F1: with no editor table a logo material must keep the startup row\n"), ++bad;
    }
    read_rule::PublishEffectTable(nullptr);
  }
  std::printf("%s eya_unit: Param1 = (g_fFurLevel, 0, g_fDifAmbLevel, 0) per material\n", bad ? "FAIL" : "PASS");
  if (plant) { std::printf("%s: the planted twin (zero list kept) %s\n", bad ? "PASS" : "FAIL", bad ? "was caught" : "was NOT caught"); return bad ? 0 : 1; }
  return bad ? 1 : 0;
}
