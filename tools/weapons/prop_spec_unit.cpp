// S111 PROPSPEC unit (docs\research\s111\PROPSPEC.md): the game's run-time specular / reflection override of an entrance's props (src\scene_live\prop_spec.h), against a fake guest. 0 game runs, no SDK.
//   out\weapons\prop_spec_unit.exe   the store per bind / release, the row bits, the drawer's values, the hook / call sites in the shipped sources; the planted twin (the drawer before the lane: the file's
//                                     values on every event prop) must give the wrong belt under the off set
// Build + run: pwsh -NoProfile -File tools\weapons\build_prop_spec.ps1
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../../src/scene_live/prop_spec.h"

namespace ps = rdr_scene::live::propspec;

namespace {

int g_bad = 0;
void Check(bool ok, const char* name, const std::string& note = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, note.c_str());
  g_bad += ok ? 0 : 1;
}

// The fake guest: words (host values of the big-endian words) and byte strings.
struct Guest {
  std::map<uint32_t, uint32_t> words;
  std::map<uint32_t, std::string> strs;
  bool Word(uint32_t a, uint32_t& v) const {
    const auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool Bytes(uint32_t a, void* dst, size_t n) const {
    const auto it = strs.find(a);
    if (it == strs.end()) return false;
    std::memset(dst, 0, n);
    std::memcpy(dst, it->second.data(), it->second.size() < n ? it->second.size() : n);
    return true;
  }
  void Float(uint32_t a, float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
};

constexpr uint32_t kNameLev = 0x8200A5A8, kNamePow = 0x8200A598, kNameRefl = 0x8200A588;   // the game's three strings
constexpr uint32_t kValue = 0x7000F000;   // the caller's stack float (sp+80 / +84 / +88)

struct Rig {
  Guest g;
  ps::Store s;
  Rig() { g.strs[kNameLev] = "g_fSpecularLev", g.strs[kNamePow] = "g_iSpecularPow", g.strs[kNameRefl] = "g_fReflectAlpha"; }
  bool Bind(uint32_t weapon, uint32_t name, float v, ps::BindSeen* seen = nullptr) {
    g.Float(kValue, v);
    return ps::OnBind(s, [&](uint32_t a, uint32_t& w) { return g.Word(a, w); }, [&](uint32_t a, void* d, size_t n) { return g.Bytes(a, d, n); }, weapon, name, kValue, seen);
  }
  bool BindRaw(uint32_t weapon, uint32_t name, uint32_t value) {
    return ps::OnBind(s, [&](uint32_t a, uint32_t& w) { return g.Word(a, w); }, [&](uint32_t a, void* d, size_t n) { return g.Bytes(a, d, n); }, weapon, name, value);
  }
  bool Release(uint32_t ovr) { return ps::OnRelease(s, [&](uint32_t a, uint32_t& w) { return g.Word(a, w); }, [&](uint32_t a, void* d, size_t n) { return g.Bytes(a, d, n); }, ovr); }
  void Weapon(uint32_t weapon, uint32_t model, uint32_t prop_id) { g.words[weapon + ps::kWeaponOffModel] = model, g.words[weapon + ps::kWeaponOffId] = prop_id; }
  void Override(uint32_t ovr, uint32_t weapon, uint32_t rec, const char* name) { g.words[ovr + ps::kOvrOffWeapon] = weapon, g.words[ovr + ps::kOvrOffRecord] = rec, g.strs[rec] = name; }
};

// A belt-like stored model: the stock BOBJ 2004 entry 0 materials (yProp_BumpMap, level 2, powers 50 / 20 / 5, reflection 1; q asset pac evtobj 352 0).
assets::Model Belt() {
  assets::Model m;
  const float pw[3] = {50.f, 20.f, 5.f};
  for (float p : pw) {
    assets::Material mat;
    mat.shader = "yProp_BumpMap", mat.spec_lev = 2.f, mat.spec_pow = p, mat.reflect_alpha = 1.f;
    m.materials.push_back(mat);
  }
  return m;
}
bool AllAre(const assets::Model& m, float lev, float pw, float refl) {
  for (const assets::Material& mat : m.materials)
    if (!ps::Near(mat.spec_lev, lev) || !ps::Near(mat.spec_pow, pw) || !ps::Near(mat.reflect_alpha, refl)) return false;
  return !m.materials.empty();
}

// The drawer's bound copy, as rf_props.cpp builds it (BindModel copies the stored model, then the set goes on it).
assets::Model NewDrawer(const assets::Model& stored, uint32_t slot_flags) {
  assets::Model bound = stored;
  const uint32_t code = ps::SpecCodeOf(slot_flags);
  if (code) ps::ApplyToModel(code, bound);
  return bound;
}
// The planted twin: the drawer before the lane (the file's values on every event prop, whatever the game bound).
assets::Model OldDrawer(const assets::Model& stored, uint32_t) { return stored; }

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

}  // namespace

int main() {
  Check(ps::NameIndex("g_fSpecularLev\0\0") == ps::kLev && ps::NameIndex("g_iSpecularPow\0\0") == ps::kPow && ps::NameIndex("g_fReflectAlpha") == ps::kRefl && ps::NameIndex("g_fSpecLev\0\0\0\0\0\0") < 0,
        "names.three_and_only_three");

  // ---- the off set (entrance bank 104): a belt's model gets 0 / 0 / 0
  Rig r;
  constexpr uint32_t kBeltW = 0x40001000, kBeltM = 0x40002000, kStepsW = 0x40003000, kStepsM = 0x40004000;
  r.Weapon(kBeltW, kBeltM, 2004), r.Weapon(kStepsW, kStepsM, 32);
  ps::BindSeen seen;
  r.Bind(kBeltW, kNameLev, 0.f), r.Bind(kBeltW, kNamePow, 0.f);
  Check(r.s.CodeFor(kBeltM) == ps::kSpecNone, "bind.partial_is_none", "two of three bound: code " + std::to_string(r.s.CodeFor(kBeltM)));
  r.Bind(kBeltW, kNameRefl, 0.f, &seen);
  Check(r.s.CodeFor(kBeltM) == ps::kSpecOff && seen.model == kBeltM && seen.prop_id == 2004 && seen.code == ps::kSpecOff, "bind.off_set",
        "code " + std::to_string(r.s.CodeFor(kBeltM)) + " prop " + std::to_string(seen.prop_id));
  // ---- the steps set: 0.3 / 40 / 0.3
  r.Bind(kStepsW, kNameLev, 0.3f), r.Bind(kStepsW, kNamePow, 40.f), r.Bind(kStepsW, kNameRefl, 0.3f);
  Check(r.s.CodeFor(kStepsM) == ps::kSpecSteps && r.s.PropIdOf(kStepsM) == 32, "bind.steps_set", "code " + std::to_string(r.s.CodeFor(kStepsM)));
  // ---- the game's early outs (sub_82335070 +60 / +68 / +70) and an unknown name bind nothing
  constexpr uint32_t kOtherW = 0x40005000, kOtherM = 0x40006000;
  r.Weapon(kOtherW, kOtherM, 2005);
  r.g.strs[0x8200B000] = "g_fSpecLev";
  r.g.Float(kValue, 5.f);
  const bool none = !r.BindRaw(0, kNameLev, kValue) && !r.BindRaw(kOtherW, 0, kValue) && !r.BindRaw(kOtherW, kNameLev, 0) && !r.Bind(kOtherW, 0x8200B000, 5.f);
  Check(none && r.s.CodeFor(kOtherM) == ps::kSpecNone && r.s.Count().live == 2, "bind.early_outs_bind_nothing", std::to_string(r.s.Count().live) + " models under a value");
  // ---- values the caller never passes: coded Other, drawn as the file
  r.Bind(kOtherW, kNameLev, 1.f), r.Bind(kOtherW, kNamePow, 9.f), r.Bind(kOtherW, kNameRefl, 1.f);
  const assets::Model other = NewDrawer(Belt(), ps::PackSpec(ps::kSpecOther));
  Check(r.s.CodeFor(kOtherM) == ps::kSpecOther && other.materials[0].spec_lev == 2.f && other.materials[0].spec_pow == 50.f && other.materials[0].reflect_alpha == 1.f, "bind.other_values_drawn_as_file");

  // ---- the release (sub_82334AB8 per override): one name back = not a whole set; all three = gone
  constexpr uint32_t kO1 = 0x50000000, kO2 = 0x50001000, kO3 = 0x50002000;
  r.Override(kO1, kBeltW, 0x51000000, "g_fSpecularLev"), r.Override(kO2, kBeltW, 0x51001000, "g_iSpecularPow"), r.Override(kO3, kBeltW, 0x51002000, "g_fReflectAlpha");
  r.Release(kO1);
  Check(r.s.CodeFor(kBeltM) == ps::kSpecNone, "release.one_name_ends_the_set");
  r.Release(kO2), r.Release(kO3);
  Check(r.s.Count().live == 2, "release.all_three_forgets_the_model", std::to_string(r.s.Count().live) + " models left (steps + other)");
  // a model leaving the event list (EventProps OnFree / Prune / OnMake) drops its set
  r.s.Drop(kStepsM);
  Check(r.s.CodeFor(kStepsM) == ps::kSpecNone && r.s.Count().live == 1, "drop.freed_model_keeps_nothing");

  // ---- the row bits do not touch the bits weapon_rows.h uses (0-29): event entry, shadow, gm / blood, stage
  const uint32_t taken = 0x3FFFFFFFu;
  Check((ps::PackSpec(3) & taken) == 0 && ps::SpecCodeOf(ps::PackSpec(ps::kSpecOff) | taken) == ps::kSpecOff && ps::SpecCodeOf(taken) == 0, "row.bits_30_31_only");

  // ---- the drawer: the game's values per set
  Check(AllAre(NewDrawer(Belt(), ps::PackSpec(ps::kSpecOff)), 0.f, 0.f, 0.f), "draw.off_set_belt_matte");
  Check(AllAre(NewDrawer(Belt(), ps::PackSpec(ps::kSpecSteps)), 0.3f, 40.f, 0.3f), "draw.steps_set");
  Check(NewDrawer(Belt(), 0).materials[1].spec_pow == 20.f && NewDrawer(Belt(), 0).materials[0].spec_lev == 2.f, "draw.no_set_file_values");
  Check(ps::PoolSuffix(ps::kSpecOff) != ps::PoolSuffix(ps::kSpecNone) && ps::PoolSuffix(ps::kSpecSteps) != ps::PoolSuffix(ps::kSpecOff) && ps::PoolSuffix(ps::kSpecNone).empty() &&
            ps::PoolSuffix(ps::kSpecOther).empty(),
        "draw.own_pool_per_set");

  // ---- the shipped sources hold the hooks and the call sites
  const std::filesystem::path src = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "src";
  const std::string origin = ReadText(src / "draw_origin.h"), glue = ReadText(src / "scene_live" / "weapon_rows_glue.h"), core = ReadText(src / "scene_live" / "weapon_rows_core.h"),
                    props = ReadText(src / "renderer_scene" / "rf_props.cpp");
  const bool hooks = origin.find("REX_HOOK_RAW(sub_82335070)") != std::string::npos && origin.find("REX_HOOK_RAW(sub_82334AB8)") != std::string::npos &&
                     glue.find("inline void PropSpecBind(") != std::string::npos && glue.find("inline void PropSpecRelease(") != std::string::npos;
  const bool row = core.find("if (!is_stage && !is_gm && !is_blood) e.slot_flags |= propspec::PackSpec(propspec::TheStore().CodeFor(model))") != std::string::npos && core.find("propspec::DropOnFree(m)") != std::string::npos &&
                   core.find("propspec::DropOnMake(model)") != std::string::npos;
  const bool draw = props.find("live::propspec::ApplyToModel(spec_code, bound)") != std::string::npos && props.find("live::propspec::PoolSuffix(spec_code)   // S105 PLATE") != std::string::npos &&
                    props.find("std::string()) + live::propspec::PoolSuffix(spec_code)") != std::string::npos && props.find("(it.stage || gb) ? 0u : live::propspec::SpecCodeOf(w.slot_flags)") != std::string::npos;   // review: the m2 steps row too
  Check(hooks && row && draw, "sources.hooks_row_drawer", std::string("hooks ") + (hooks ? "ok" : "MISSING") + ", row " + (row ? "ok" : "MISSING") + ", drawer " + (draw ? "ok" : "MISSING"));

  // ---- the planted twin: the drawer before the lane draws the bank-104 belt with the file's shine
  const assets::Model twin = OldDrawer(Belt(), ps::PackSpec(ps::kSpecOff));
  const bool caught = !AllAre(twin, 0.f, 0.f, 0.f);
  std::printf("      twin: belt level %.2f power %.0f reflection %.2f under the off set (the game: 0 / 0 / 0)\n", double(twin.materials[0].spec_lev), double(twin.materials[0].spec_pow),
              double(twin.materials[0].reflect_alpha));
  Check(caught, "twin.old_drawer_caught", "the old drawer keeps the file's level 2 / reflection 1 where the game binds 0");

  std::printf("%s prop_spec_unit: the entrance props' run-time specular / reflection sets follow the game's binder and release\n", g_bad ? "FAIL" : "PASS");
  return g_bad ? 1 : 0;
}
