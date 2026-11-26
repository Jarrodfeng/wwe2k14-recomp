// Session 101, lane 13: the scene front-end's character path. See char_effects.h and docs\renderer\SCENE_FRONTEND.md "Wrestlers".
#include "char_effects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <atomic>   // S103 L7
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"
#include "read_rule.h"        // S103 YCB: the effect table
#include "technique_pick.h"   // S103 YCB: RowOf
#include "logo_bool.h"        // S105 A5c: bit 150
#include "../assets/load_plants.h"   // S103 LNT: EyaOldParam1Plant
#include "../scene_live/change_colour.h"   // S108 THR2: MaskIndexOfSlot, kColours (pure)
#include "sphere_join.h"            // S111 SPHEREOCC: kNeutral (the block a body with no row is fed)

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
constexpr int kVariantCount = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));

// The wrestler effects of the registry (the .fx.json "effect" of every yCh_*). A variant name is "<effect>_<variant>_vertex|_fragment"; the
// longest effect name that is a prefix owns it (yCh_skin_flex is not yCh_skin, yCh_cloth_trans is not yCh_cloth).
const char* const kEffects[] = {"yCh_Aniso_trans", "yCh_Anisotropic", "yCh_Ls",         "yCh_Reflect", "yCh_TransTiling", "yCh_cloth_flex", "yCh_cloth_trans",
                                "yCh_cloth",       "yCh_gear_flex",   "yCh_gear",       "yCh_skin_flex", "yCh_skin",       "yCh_trans"};

std::string OwnerOf(const std::string& variant) {
  std::string best;
  for (const char* e : kEffects) {
    const std::string prefix = std::string(e) + "_";
    if (variant.compare(0, prefix.size(), prefix) == 0 && std::strlen(e) > best.size()) best = e;
  }
  return best;
}

struct Tables {
  std::vector<const fxt::VariantTable*> by_index;
  Tables() {
    std::unordered_map<std::string, const fxt::VariantTable*> named;
    for (const fxt::VariantTable& t : fxt::kTables) named.emplace(t.variant, &t);
    by_index.assign(size_t(kVariantCount), nullptr);
    for (int i = 0; i < kVariantCount; ++i) {
      if (!kVariants[i].name) continue;
      const auto it = named.find(kVariants[i].name);
      if (it != named.end()) by_index[size_t(i)] = it->second;
    }
  }
};
const Tables& T() {
  static const Tables tables;
  return tables;
}

std::vector<std::string> Defines(const fxt::VariantTable& t) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* p = t.defines; p && *p; ++p) {
    if (*p == ' ') { if (!cur.empty()) out.push_back(cur), cur.clear(); }
    else cur += *p;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool Subset(const std::vector<std::string>& have, std::initializer_list<const char*> allowed) {
  for (const std::string& d : have) {
    bool ok = false;
    for (const char* a : allowed) ok = ok || d == a;
    if (!ok) return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& have, const char* d) { return std::find(have.begin(), have.end(), d) != have.end(); }

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool SupportedCharInput(const char* name) {
  static const char* const kOk[] = {"inPosition", "inNormal", "inUv", "inTangent", "inBoneId", "inBoneIds", "inWeights", "inExtra"};
  for (const char* n : kOk)
    if (std::strcmp(name, n) == 0) return true;
  return false;
}

}  // namespace

bool IsCharEffect(const std::string& effect) { return effect.compare(0, 4, "yCh_") == 0; }

bool IsClothEffect(const std::string& effect) { return effect.compare(0, 9, "yCh_cloth") == 0; }

bool IsHairEffect(const std::string& effect) {
  std::string lc = effect;
  for (char& ch : lc) ch = char(std::tolower((unsigned char)ch));
  return technique_pick::CharClassOf(lc).hair;   // S103 YCB (review P2-2): exactly ych_trans / ych_aniso_trans (was a substring test that also took yCh_Anisotropic, cloth_trans, TransTiling)
}

CharSlot RoleOfSlot(int slot) {
  switch (slot) {
    case 0: return CharSlot::Diffuse;
    case 1: return CharSlot::Normal;
    case 3: return CharSlot::NoiseTile;
    case 6: return CharSlot::Mask;
    case 10: return CharSlot::EnvCube;   // S111 ENVCUBE: g_mEnvLPrm (every yCh fragment that samples 10 defines FX_ENV_LIGHT: envcube_unit)
    case 12: return CharSlot::ShadowMap;   // S103 L6
    case 13: return CharSlot::LightCube;
    case 15: return CharSlot::RimMap;
    case 5: return CharSlot::WrinkleNormal;   // S103 WRK: g_mNormalWklPrm (name-table row 0x8392AB60, register 5)
    case 8: return CharSlot::HairNoise;   // S111 HAIRMAPS: g_mHairNoisePrm (row 0x8392ABC0, register 8)
    case 9: return CharSlot::HairShift;   // S111 HAIRMAPS: g_mHairShiftPrm (row 0x8392ABE0, register 9)
    case 4: return CharSlot::Logo4;   // S105 A5c
    case 7: return CharSlot::Logo7;
    default: return CharSlot::FlatBlack;  // 2, 14: team-colour masks
  }
}

// ---- S103 YCB: the table pick (S111 OLDCHAIN: the only pick; the min-define chooser that stood behind it is gone) ------------------------------------------------------------------------------

namespace {
// The registry variant of `effect` (owner test, not a name prefix: yCh_skin_ must not take a yCh_skin_flex_ variant) with microcode hash `h` and name suffix `suffix` ("_vertex" / "_fragment"); -1 = none.
int OwnVariantOfHash(uint64_t h, const std::string& effect, const char* suffix) {
  const auto* first = std::begin(rdr::fx::kHashes);
  const auto* last = std::end(rdr::fx::kHashes);
  const auto* it = std::lower_bound(first, last, h, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
  for (; it != last && it->hash == h; ++it) {
    const char* n = kVariants[it->variant].name;
    if (n && T().by_index[size_t(it->variant)] && EndsWith(n, suffix) && OwnerOf(n) == effect) return int(it->variant);
  }
  return -1;
}

// S103 YCB (CUB.md Route A): the fragment of the same owner whose defines are the cube variant's minus FX_CUBE_SPECULAR; -1 = none.
int CubeSibling(int f) {
  std::vector<std::string> want = Defines(*T().by_index[size_t(f)]);
  want.erase(std::remove(want.begin(), want.end(), std::string("FX_CUBE_SPECULAR")), want.end());
  std::sort(want.begin(), want.end());
  const std::string owner = OwnerOf(kVariants[f].name);
  for (int i = 0; i < kVariantCount; ++i) {
    const char* n = kVariants[i].name;
    if (i == f || !n || !T().by_index[size_t(i)] || !EndsWith(n, "_fragment") || OwnerOf(n) != owner) continue;
    std::vector<std::string> d = Defines(*T().by_index[size_t(i)]);
    std::sort(d.begin(), d.end());
    if (d == want) return i;
  }
  return -1;
}

enum class PsBind { Ok, Cube, Wrinkles, Other };
PsBind CheckPs(int f, bool alpha_test, int pass, std::string& why) {
  const rdr::fx::Variant& fv = kVariants[f];
  const std::vector<std::string> fdef = Defines(*T().by_index[size_t(f)]);
  bool cube = false, other = false;
  for (uint32_t s = 0; s < fv.sampler_count; ++s) {
    const int slot = fv.samplers[s].slot, kind = fv.samplers[s].kind;
    if (slot == 11) { cube = true; continue; }   // the per-material spec-power cube (GPU-written in the game): not fed in the stream-free path
    const bool known = (slot >= 0 && slot <= 10) || slot == 12 || slot == 13 || slot == 14 || slot == 15;
    if (!known || (kind != 0 && !((slot == 13 || slot == 10) && kind == 1))) other = true;
    if (slot == 12 && !(pass & 2)) other = true;   // a shadow-map reader in a pass with no shadow map
  }
  if (cube) return why = "reads the cube-specular slot 11", PsBind::Cube;
  if (Has(fdef, "FX_WRINKLES") && !assets::g_wrk_live.load(std::memory_order_relaxed)) return why = "reads the wrinkle map (slot 5, source unsettled)", PsBind::Wrinkles;
  if (other) return why = "a sampler slot the front end cannot feed", PsBind::Other;
  if (alpha_test && !fv.spirv_alpha) return why = "no alpha-test variant", PsBind::Other;
  return PsBind::Ok;
}

// The vertex variant `v` can be bound for a draw whose fragment is `f`: the registry tables exist, supported inputs, no vertex samplers, every fragment interpolator written, the skin flag agrees.
bool CheckVs(int v, int f, bool skinned, std::string& why) {
  const fxt::VariantTable& vt = *T().by_index[size_t(v)];
  const fxt::VariantTable& ft = *T().by_index[size_t(f)];
  const std::vector<std::string> vdef = Defines(vt);
  if (!Subset(vdef, {"FX_SKINNED", "FX_VIEW_NORMAL", "FX_SHADOW", "FX_SPHERE_OCC", "FX_ID_MAP", "FX_WRINKLE_LEVEL"})) return why = "the vertex variant has an option this path does not feed", false;
  if (Has(vdef, "FX_SKINNED") != skinned) return why = "the vertex variant's skin flag differs from the draw's", false;
  if (kVariants[v].sampler_count) return why = "the vertex variant samples", false;
  for (uint32_t k = 0; k < vt.input_count; ++k)
    if (!SupportedCharInput(vt.inputs[k].name)) return why = "a vertex input this path does not feed", false;
  for (uint32_t k = 0; k < ft.interp_count; ++k) {
    bool found = false;
    for (uint32_t j = 0; j < vt.interp_count; ++j) found = found || vt.interps[j].location == ft.interps[k].location;
    if (!found) return why = "a fragment interpolator the vertex stage does not write", false;
  }
  return true;
}
}  // namespace

CharChoice ChooseCharEffectTable(const std::string& effect, bool skinned, bool alpha_test, uint32_t technique, int pass, bool logo, bool change_colour) {
  CharChoice out;
  out.effect = effect;
  auto table = read_rule::EffectTableNow();
  if (!table) return out.no_table = true, out.why_not = "no effect table yet", out;
  if (change_colour) {   // S108 THR2: a recoloured body draws from the Threads editor's table (edit.pac p9/0 = the game's model+392 row while model+604 is set)
    const auto st = read_rule::StsEffectTableNow();
    if (st && st->count(read_rule::Lower(effect))) table = st, out.change_colour_table = true;
  }
  if (logo && !out.change_colour_table) {   // S105 A5c fix F1: a logo material draws from the editor's table (edit.pac p7/0), as the game's editor does
    const auto lt = read_rule::LogoEffectTableNow();
    if (lt && lt->count(read_rule::Lower(effect))) table = lt, out.logo_table = true;
  }
  if (pass < 0) return out.why_not = "no pass", out;   // S111 OLDCHAIN review: no upper cap of ours: RowOf bounds the pass by the technique's own rows (TECHPASS's +16 passes p17 / p19 bind their row or are a counted hole)
  {
    const int p = pass;
    const read_rule::PassShaders* row = technique_pick::RowOf(*table, effect, technique, uint32_t(p));
    // Our rigid stream needs the rigid vertex variant: the VS of row `p & ~1` (the game's rigid parts draw the skinned row, F2); the pixel shader is row `p`'s.
    const read_rule::PassShaders* vrow = skinned ? row : technique_pick::RowOf(*table, effect, technique, uint32_t(p & ~1));
    if (!row || !row->ps_ok || !vrow || !vrow->vs_ok) return out.why_not = "no table row for technique " + std::to_string(technique) + " pass " + std::to_string(p), out;
    int f = OwnVariantOfHash(row->ps, effect, "_fragment");
    const int v = OwnVariantOfHash(vrow->vs, effect, "_vertex");
    if (f < 0 || v < 0) return out.why_not = std::string("the table's ") + (f < 0 ? "pixel" : "vertex") + " shader is not in the registry", out;
    std::string why;
    PsBind r = CheckPs(f, alpha_test, p, why);
    if (r == PsBind::Cube) {
      // S103 YCB / CUB.md Route A [C]: slot 11 is the bake of sum_i w_i max(d.B_i, 0)^P / sum_i w_i, which times DifLghMax.w (= sum w) is exactly the non-cube branch our shaders evaluate per pixel. The cube variant
      // differs from its non-cube sibling only in View (c52-54) and slot 11, so the row binds the variant of the SAME owner with the SAME defines minus FX_CUBE_SPECULAR (the row's own vertex stage stays).
      // A deliberate rule-6 choice (the 360 bake is dropped, 360_HABITS), counted char_cube_analytic: not a fallback.
      const int sib = CubeSibling(f);
      if (sib < 0) return out.why_not = "technique " + std::to_string(technique) + " pass " + std::to_string(p) + ": the cube row has no non-cube sibling in the registry", out;
      f = sib, r = CheckPs(f, alpha_test, p, why), out.cube_analytic = true;
    }
    if (r != PsBind::Ok) {
      out.cube_analytic = false;
      out.blocked_wrinkles = r == PsBind::Wrinkles;
      return out.why_not = "technique " + std::to_string(technique) + " pass " + std::to_string(p) + ": " + why, out;
    }
    if (!CheckVs(v, f, skinned, why)) return out.cube_analytic = false, out.why_not = "technique " + std::to_string(technique) + " pass " + std::to_string(p) + ": " + why, out;
    out.vs = v, out.ps = f, out.tech_picked = true;
    const std::vector<std::string> fdef = Defines(*T().by_index[size_t(f)]), vdef = Defines(*T().by_index[size_t(v)]);
    out.shadow = Has(fdef, "FX_SHADOW") || Has(vdef, "FX_SHADOW");
    for (uint32_t s = 0; s < kVariants[f].sampler_count; ++s) out.wet = out.wet || kVariants[f].samplers[s].slot == 14;
    if (out.change_colour_table) out.wet = false;   // S108 THR2: on a change-colour draw slot 14 is Mask3 (sub_8339F0E0 binds it), never the liquid map
    return out;
  }
}

CharSlot RoleOfWetSlot(int slot) {   // S103 SWF
  if (slot == 4) return CharSlot::SweatBump;
  if (slot == 14) return CharSlot::LiquidMap;
  return RoleOfSlot(slot);
}

CharSlot RoleOfChangeSlot(int slot) {   // S108 THR2
  const int k = live::change_colour::MaskIndexOfSlot(slot);
  if (k >= 0) return CharSlot(int(CharSlot::ChangeMask1) + k);
  return RoleOfSlot(slot);
}

// ---- the ambient cube ----------------------------------------------------------------------------------------------------------

namespace {

uint16_t ToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t m = x & 0x7FFFFFu;
  if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00u | (m ? 0x200u : 0u));
  if (e >= 31) return uint16_t(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return uint16_t(sign);
    m |= 0x800000u;
    const uint32_t shift = uint32_t(14 - e);
    uint32_t h = m >> shift;
    if ((m >> (shift - 1)) & 1u) ++h;
    return uint16_t(sign | h);
  }
  uint32_t h = (uint32_t(e) << 10) | (m >> 13);
  if (m & 0x1000u) ++h;
  return uint16_t(sign | h);
}

float Saturate(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
float Sign(float x) { return x > 0.f ? 1.f : (x < 0.f ? -1.f : 0.f); }

// viewer_hlsl.h PLLobe: a quadratic Bezier lobe of the cosine, with the sign of which side of the threshold it is on.
float PLLobe(float c, float thr, float width, float root_off, float off, float scale, float& side) {
  const float s = c - thr;
  const float folded = Saturate(std::fabs(s) + thr);
  const float m = -width * (1.f - folded) + root_off;
  side = Sign(s);
  const float t = (-std::sqrt(std::fabs(m)) - off) * scale;
  return (1.f - 0.5f * t * t) - 0.5f;
}

}  // namespace

uint64_t LightSetKey(const assets::LightSet& ls) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(ls.dir, sizeof ls.dir), mix(ls.col, sizeof ls.col), mix(ls.pcol, sizeof ls.pcol), mix(ls.fall, sizeof ls.fall);
  mix(&ls.has_pcol, sizeof ls.has_pcol);
  return h;
}

void BuildAmbientCube(const assets::LightSet& ls, int size, std::vector<uint16_t>& half, float scale[4]) {
  float pc[4][3], lum[4];
  float sc[3] = {0, 0, 0}, sp[3] = {0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    for (int c = 0; c < 3; ++c) {
      pc[k][c] = ls.has_pcol ? ls.pcol[k][c] : ls.col[k][c];
      sc[c] += ls.col[k][c], sp[c] += pc[k][c];
    }
    lum[k] = pc[k][0] * 0.755379379f + pc[k][1] * 0.236693099f + pc[k][2] * 0.0079275202f;
  }
  for (int c = 0; c < 3; ++c) scale[c] = sp[c] > 1e-6f ? sc[c] / sp[c] : 1.f;
  scale[3] = scale[2];
  half.assign(size_t(6) * size_t(size) * size_t(size) * 4, 0);
  for (int face = 0; face < 6; ++face)
    for (int j = 0; j < size; ++j)
      for (int i = 0; i < size; ++i) {
        const float sc_ = 2.f * (float(i) + 0.5f) / float(size) - 1.f, tc = 2.f * (float(j) + 0.5f) / float(size) - 1.f;
        float d[3];
        switch (face) {
          case 0: d[0] = 1, d[1] = -tc, d[2] = -sc_; break;
          case 1: d[0] = -1, d[1] = -tc, d[2] = sc_; break;
          case 2: d[0] = sc_, d[1] = 1, d[2] = tc; break;
          case 3: d[0] = sc_, d[1] = -1, d[2] = -tc; break;
          case 4: d[0] = sc_, d[1] = -tc, d[2] = 1; break;
          default: d[0] = -sc_, d[1] = -tc, d[2] = -1; break;
        }
        const float il = 1.f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& x : d) x *= il;
        float w1[4], s1[4], w2[4], s2[4];
        for (int k = 0; k < 4; ++k) {
          const float c = ls.dir[k][0] * d[0] + ls.dir[k][1] * d[1] + ls.dir[k][2] * d[2];
          w1[k] = PLLobe(c, 0.166666701f, 4.f * -0.333333403f, 0.25f, -0.5f, -1.5f, s1[k]);
          w2[k] = PLLobe(c, 0.409090906f, 4.f * -0.4909091f, 0.00999999978f, -0.100000001f, -1.01851797f, s2[k]);
        }
        float k2[4], p[4], q[4], e2[4], e1[4], pl[4];
        float total = 0.f, mx = 0.f;
        for (int k = 0; k < 4; ++k) {
          k2[k] = w2[k] * s2[k] + 0.5f;
          p[k] = w1[k] * s1[k] + 0.5f;
          q[k] = w1[k] * s1[k] + (w2[k] * s2[k] + 1.f);
          e2[k] = q[k] * 0.5f - k2[k];
          e1[k] = q[k] * 0.5f - p[k];
          pl[k] = p[k] * lum[k];
          total += pl[k];
          mx = std::max(mx, pl[k]);
        }
        const float share = total > 0.f ? mx / total : 0.f;
        float rgb[3] = {0, 0, 0}, glow = 0.f;
        for (int k = 0; k < 4; ++k) {
          const float keep = 1.f - share * (1.f - ls.fall[k]);
          const float shaded = e1[k] * keep + p[k], weight = e2[k] * keep + k2[k];
          glow += shaded * lum[k];
          for (int c = 0; c < 3; ++c) rgb[c] += weight * pc[k][c];
        }
        uint16_t* px = &half[((size_t(face) * size_t(size) + size_t(j)) * size_t(size) + size_t(i)) * 4];
        px[0] = ToHalf(rgb[0]), px[1] = ToHalf(rgb[1]), px[2] = ToHalf(rgb[2]), px[3] = ToHalf(glow);
      }
}

// ---- skin chunks ---------------------------------------------------------------------------------------------------------------

bool BuildSkinChunks(const assets::Mesh& mesh, std::vector<SkinChunk>& out) {
  out.clear();
  const size_t tris = mesh.indices.size() / 3;
  const auto nodes_of = [&](uint32_t vi, uint16_t* n, int& count) {
    const assets::Vertex& v = mesh.vertices[vi];
    count = 0;
    for (int k = 0; k < 4; ++k) {
      if (k > 0 && !(v.weights[k] > 0.f)) continue;
      bool dup = false;
      for (int i = 0; i < count; ++i) dup = dup || n[i] == v.bones[k];
      if (!dup) n[count++] = v.bones[k];
    }
  };
  SkinChunk* cur = nullptr;
  std::unordered_map<uint32_t, uint32_t> vmap;
  const auto finish_chunk_if_needed = [&](const uint16_t* need, int need_count) -> bool {
    int fresh = 0;
    if (cur) for (int i = 0; i < need_count; ++i) fresh += std::find(cur->nodes.begin(), cur->nodes.end(), need[i]) == cur->nodes.end();
    if (!cur || int(cur->nodes.size()) + fresh > kMaxPaletteBones) {
      if (need_count > kMaxPaletteBones) return false;
      out.emplace_back();
      cur = &out.back();
      vmap.clear();
    }
    return true;
  };
  for (size_t t = 0; t < tris; ++t) {
    uint16_t need[12];
    int need_count = 0;
    for (int c = 0; c < 3; ++c) {
      uint16_t n[4];
      int cnt;
      nodes_of(mesh.indices[t * 3 + size_t(c)], n, cnt);
      for (int i = 0; i < cnt; ++i) {
        bool dup = false;
        for (int j = 0; j < need_count; ++j) dup = dup || need[j] == n[i];
        if (!dup) need[need_count++] = n[i];
      }
    }
    if (!finish_chunk_if_needed(need, need_count)) return false;
    for (int i = 0; i < need_count; ++i)
      if (std::find(cur->nodes.begin(), cur->nodes.end(), need[i]) == cur->nodes.end()) cur->nodes.push_back(need[i]);
    for (int c = 0; c < 3; ++c) {
      const uint32_t vi = mesh.indices[t * 3 + size_t(c)];
      auto it = vmap.find(vi);
      if (it == vmap.end()) {
        const assets::Vertex& v = mesh.vertices[vi];
        SkinVertex sv{};
        const auto slot_of = [&](uint16_t node) { return uint32_t(std::find(cur->nodes.begin(), cur->nodes.end(), node) - cur->nodes.begin()); };
        const uint32_t first = slot_of(v.bones[0]);
        for (int k = 0; k < 4; ++k) sv.ids[k] = (k == 0 || v.weights[k] > 0.f) ? slot_of(v.bones[k]) : first;
        float wsum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
        if (!(wsum > 0.f)) sv.w[0] = 1.f;  // (a vertex flagged skinned with no weights: bone 0 alone)
        else for (int k = 0; k < 4; ++k) sv.w[k] = v.weights[k];
        it = vmap.emplace(vi, uint32_t(cur->vertices.size())).first;
        cur->vertices.push_back(v);
        cur->skin.push_back(sv);
        cur->src.push_back(uint32_t(vi));   // S103 W2
      }
      cur->indices.push_back(it->second);
    }
  }
  return true;
}

// ---- constants by name ---------------------------------------------------------------------------------------------------------

// S103 L7: the wrestlers' key light is CAMERA-ATTACHED. The record carries it in view space (brock_losswin: (0, 0.643, -0.766) = (0, sin 40, -cos 40) in every frame); the game feeds the pixel stage
// DifLghVec[4] / KeyLightVec = right * d.x + down * d.y + forward * d.z, the camera's axes in the game's space (down = minus the clip-y row, forward = the clip-w row).
// [C] residual 0.0003 against the stream's KeyLightVec at brock_losswin f110 (-0.2483, 0.5777, -0.7776) and f200 (-0.4326, 0.5673, -0.7007), from that frame's own ViewProj registers (c56..c59,
// orthonormalised). d.x is 0 in every capture seen, so the SIGN of the right axis is [U] (assumed +). Only a record's light set (LightSet::hl_view, set by rf_wrestlers.cpp) is rotated: the viewer's entrance path
// (assets::entrance::RotateDirs) keeps its own world values. WWE_RDR_NO_KEY_ATTACH=1 (dev plant) = the old behaviour, the authored vector unrotated.
static std::atomic<uint64_t> g_key_resolves{0}, g_key_with_x{0};
void KeyLightCensus(uint64_t& resolves, uint64_t& with_x) { resolves = g_key_resolves.load(std::memory_order_relaxed), with_x = g_key_with_x.load(std::memory_order_relaxed); }

static bool KeyLightWorld(const CharCtx& c, const float* d, float* o) {
  static const bool off = std::getenv("WWE_RDR_NO_KEY_ATTACH") != nullptr;
  if (off || !c.view_proj || !c.lights || !c.lights->hl_view) return false;
  g_key_resolves.fetch_add(1, std::memory_order_relaxed);   // the census: the right-axis sign is only testable by a key with |d.x| > 1e-3
  if (std::fabs(d[0]) > 1e-3f) g_key_with_x.fetch_add(1, std::memory_order_relaxed);
  double col[4][3];
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 3; ++j) col[k][j] = double(j == 1 ? c.mirror : 1.f) * double(c.view_proj[j * 4 + k]);   // register k xyz = column k of the row-vector view-projection, in the game's space (as the ViewProj register below)
  const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const auto unit = [&](double* a) {
    const double n = std::sqrt(dot(a, a));
    if (n > 1e-12)
      for (int j = 0; j < 3; ++j) a[j] /= n;
    return n > 1e-12;
  };
  double f[3] = {col[3][0], col[3][1], col[3][2]}, u[3], r[3];
  if (!unit(f)) return false;
  const double uf = dot(col[1], f);
  for (int j = 0; j < 3; ++j) u[j] = col[1][j] - uf * f[j];
  if (!unit(u)) return false;
  const double rf = dot(col[0], f), ru = dot(col[0], u);
  for (int j = 0; j < 3; ++j) r[j] = col[0][j] - rf * f[j] - ru * u[j];
  if (!unit(r)) return false;
  for (int j = 0; j < 3; ++j) o[j] = float(r[j] * double(d[0]) - u[j] * double(d[1]) + f[j] * double(d[2]));
  return true;
}

bool ResolveChar(const std::string& name, uint32_t vec4s, const CharCtx& c, float* out) {
  const assets::Material& mt = *c.mat;
  const assets::LightSet& ls = *c.lights;
  const auto set4 = [&](uint32_t k, float a, float b, float cc, float d) {
    if (k >= vec4s) return;
    out[4 * k] = a, out[4 * k + 1] = b, out[4 * k + 2] = cc, out[4 * k + 3] = d;
  };
  // Space: the effects take the game's file space (-Y up). Matrices that enter mirror Y once (S = diag(1, -1, 1)): the bone rows (S x world), the
  // view-projection (clip = p_file * S * VP) and the eye. Everything else is the file's own.
  if (name == "ViewProj") {
    for (uint32_t k = 0; k < 4 && k < vec4s; ++k)
      for (int i = 0; i < 4; ++i) out[4 * k + uint32_t(i)] = (i == 1 ? c.mirror : 1.f) * c.view_proj[i * 4 + int(k)];
    return true;
  }
  if (name == "WorldMatrixArray") {
    const uint32_t slots = vec4s / 3;
    for (uint32_t s = 0; s < slots; ++s) {
      const assets::Matrix& m = c.bones[std::min(s, c.bone_count ? c.bone_count - 1 : 0u)];
      for (uint32_t k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i) out[4 * (3 * s + k) + uint32_t(i)] = (k == 1 ? c.mirror : 1.f) * m[size_t(i * 4) + k] - ((c.cloth_offset && uint32_t(i) == k) ? 1.f : 0.f);
    }
    return true;
  }
  if (name == "EyePos") { set4(0, c.eye[0], c.mirror * c.eye[1], c.eye[2], 1.f); return true; }
  if (name == "FogDistance") { set4(0, 1e7f, 2e7f, 1e-7f, 0.f); return true; }  // no fog: the ramp never starts
  if (name == "MatAmbCol") { std::memcpy(out, mt.ambient, 16); return true; }
  if (name == "MatDifCol") { std::memcpy(out, mt.diffuse_color, 16); return true; }
  if (name == "MdlAlpha") { set4(0, c.alpha, 0, 0, 0); return true; }
  if (name == "NormalFlip") { set4(0, 1.f, 1.f, 1.f, 0.f); return true; }  // c218 = (1, 1, 1) in the game's draws; w = 0: the normal as stored
  // EYA plant: the EyaOldParam1Plant flag restores the zero list for Param1 (the gate lane s103_EYA proves the unit catches it).
  if (assets::load_detail::EyaOldParam1Plant() && name == "Param1") return true;
  if (name == "Param1") { set4(0, mt.fur_level, 0.f, mt.dif_amb_level, 0.f); return true; }   // EYA [C: sub_82BAAEE8 c247: g_fFurLevel -> x, g_fDifAmbLevel -> z]
  // S103 SWF: a draw bound to the wet variant reads the material's own wet constants [C SWB S2-3: PS c205.zw = (g_fBSSSwtMax, g_fBSSSwtMin), c210 = (SweatSpeLv, SweatSpePw, SweatBpCnt, SweatLevel), c234.xy =
  // (BloodSpecLv, BloodSpecPw), c246 = g_f4BSSBldClr; captured (4.0, 2.0, 0.4, 0.15) / (0.8, 100, 24, 0) / (3.0, 200, 0, 0) / (0.66, 0, 0, 1) on a00 / a04 match]; the dry draw keeps zero (byte for byte).
  if (name == "Param0") { set4(0, mt.fairy, mt.fairy_edge, c.wet ? mt.bss_swt[0] : 0.f, c.wet ? mt.bss_swt[1] : 0.f); return true; }
  // S103 YCB (CUB.md Route A): a table-picked draw also reads Sweat.y = g_f4SweatSpePw (the second sweat-highlight power the 360 baked into the slot-11 cube's .z): the analytic cube term and the gear's non-cube sibling read c210 (gear materials carry it).
  // Review F3: the 360's bake replaced a lobe power of 0 by 1 per channel (c203, sub_82C1FBE0 +0x63C..+0x69C); an analytic draw (cube_analytic) feeds the same 0 -> 1 for SpecCubPrm1.y / .w and Sweat.y.
  const auto bake_pow = [&](float v) { return (c.cube_analytic && v == 0.f) ? 1.f : v; };
  if (c.spec_sweat && !c.wet && name == "Sweat") { set4(0, mt.sweat_prm[0], bake_pow(mt.sweat_prm[1]), mt.sweat_prm[2], mt.sweat_prm[3]); return true; }
  if (c.wet) {
    if (name == "Sweat") { set4(0, mt.sweat_prm[0], bake_pow(mt.sweat_prm[1]), mt.sweat_prm[2], mt.sweat_prm[3]); return true; }
    if (name == "Blood") { set4(0, mt.blood_prm[0], mt.blood_prm[1], 0.f, 0.f); return true; }
    if (name == "BSSBldClr") { set4(0, mt.bss_bld_clr[0], mt.bss_bld_clr[1], mt.bss_bld_clr[2], mt.bss_bld_clr[3]); return true; }
  }
  // S111 SPHEREOCC: VS c168-c169 SphereOccP = (centre xyz, radius), c170-c171 SphereOccV = (axis xyz, strength) = the game's own sphere occluders of THIS body, computed by its draw node
  // right before it draws (sub_826DFCC8 -> sub_826DF8E0 -> sub_82BCC888) and uploaded by sub_82BE7AC0 whether a shadow map is read or not. c.sphere_occ = the record's newest row of the body's
  // skeleton (sphere_join.h, 16 floats P0 P1 V0 V1); none, or a sphere the game did not install = strength 0 (kNeutral).
  if (name == "SphereOccP" || name == "SphereOccV") { std::memcpy(out, (c.sphere_occ ? c.sphere_occ : sphere_join::kNeutral) + (name == "SphereOccP" ? 0 : 8), sizeof(float) * 4 * std::min<uint32_t>(vec4s, 2)); return true; }
  // S103 L6: the shadow reader's rows (the plain variants never name them). TexProjLight = MapImage::matrix (register layout): XY = c68 / c69, W = c71 (the same row as ViewLightDepth: [C] the game's
  // wrestler draw brock_losswin f110 op 377 has TexProjLightW == ViewLightDepth bit for bit). ShadowPrm = the three rows the map was made with (MapImage::prm = CasterRows::shadow_prm: the record's on the stream route, shadow_fit.h's on the fit path).
  if (c.shadow_matrix && c.shadow_params && c.shadow_prm) {
    if (name == "TexProjLightXY") { std::memcpy(out, c.shadow_matrix, vec4s >= 2 ? 32 : 16); return true; }
    if (name == "TexProjLightW" || name == "ViewLightDepth") { std::memcpy(out, c.shadow_matrix + 12, 16); return true; }
    if (name == "ShadowPrm") {
      for (uint32_t k = 0; k < 3; ++k) set4(k, c.shadow_prm[4 * k], c.shadow_prm[4 * k + 1], c.shadow_prm[4 * k + 2], c.shadow_prm[4 * k + 3]);   // from the record, nothing hard-coded
      return true;
    }
  }
  // S103 L7 / S111 RIMGAME: RimLightDepth = (near, far, z, 0) from the rim pass's depth range (MapImage::params[2..3]); z = the game's G+308 [C sub_827E9D90 +BC..+E4: far - near > 0 ? min((far - near) / 1000, 1) : 1,
  // 1000 = the rim object's +72 (sub_827EA7F0 +1C4)], carried into the model's block + 32 by sub_82BD7B50 +590..+5BC (0x82BDD108's by-reference path). Captured: brock_losswin f110 -> 0.1794. No rim map: (0, 1, 1, 0).
  if (name == "RimLightDepth") {
    if (c.rim_params) { const float d = c.rim_params[3] - c.rim_params[2]; set4(0, c.rim_params[2], c.rim_params[3], d > 0.f ? std::min(d / 1000.f, 1.f) : 1.f, 0.f); }
    else set4(0, 0.f, 1.f, 1.f, 0.f);
    return true;
  }
  // S103 B3F plant: WWE_RDR_B3F_OLD_HAIR_RIM restores the old zeros / hard-coded rim values (the gate lane s103_B3F proves feature_diff finds the gap again).
  static const bool b3f_old = std::getenv("WWE_RDR_B3F_OLD_HAIR_RIM") != nullptr;
  if (b3f_old && (name == "HairSpeParam" || name == "SpeCol1" || name == "SpeCol2")) return true;
  if (b3f_old && name == "RimLightParam3") { set4(0, 500.f, 2500.f, 1.f, 0.4f); return true; }
  if (name == "RimLightParam3") { set4(0, mt.rim_atn[0], mt.rim_atn[1], mt.rim_atn[2], mt.rim_atn[3]); return true; }  // S103 B3F: c174 = (g_fRimAtnDsMin, DsMax, RtMin, RtMax) per material [C: sub_82BAAEE8 0x82BAC0C8-0x82BAC170]
  if (name == "RimLightParam") { set4(0, 1.f, 0.83f, 1.f, 0.f); return true; }       // c161 = the rim object's constants [C RIMGAME: sub_827EA7F0 +1B4..+1CC sets +64 = 1, +68 = 1, +76 = 0.83 (0x82033C18) once; sub_827E9D90 copies (+64, +76, +68, 0) to G+320 each frame]
  float key_world[3] = {ls.hl_dir[0], ls.hl_dir[1], ls.hl_dir[2]};
  KeyLightWorld(c, ls.hl_dir, key_world);   // S103 L7: the key light is authored in view space; the game rotates it by the camera every frame
  if (name == "KeyLightColor") { set4(0, ls.hl_col[0], ls.hl_col[1], ls.hl_col[2], 1.f); return true; }
  if (name == "KeyLightVec") { set4(0, key_world[0], key_world[1], key_world[2], 0.f); return true; }
  if (name == "Bools") {  // four bones: bits 1, 2, 3 of the first word (the vertex stage); bit 150 (logo) stays off
    const uint32_t bits = 0xEu;
    std::memcpy(out, &bits, 4);
    SetLogoBool(out, vec4s, mt.diffuse_t0);   // S105 A5c: bit 150 for a part whose diffuse sampler is texture index 0 (the game's rule, logo_bool.h)
    return true;
  }
  if (name == "NoiseLodParam") { set4(0, mt.noise_dist[0], mt.noise_dist[1], 0.f, 0.f); return true; }
  if (name == "Level") { set4(0, 0.f, 0.f, 0.f, mt.tex_bis_dif); return true; }  // c201.w = the material's g_f4TexBis_Dif, the diffuse LOD bias (S111 UNIFY5: row 0x8392A480; -1 in every file, SKM run 6816); y / z (g_f4FaceLv_Wud / _Bld) no yCh shader reads
  if (name == "SkyDir") { set4(0, 0.f, 1.f, 0.f, 0.f); return true; }
  if (name == "SkyCol") { set4(0, ls.sky[0], ls.sky[1], ls.sky[2], 1.f); return true; }
  if (name == "GroundCol") { set4(0, ls.ground[0], ls.ground[1], ls.ground[2], 1.f); return true; }
  if (name == "DifLghCol") {
    for (uint32_t k = 0; k < vec4s; ++k) {
      if (k < 4) set4(k, ls.col[k][0], ls.col[k][1], ls.col[k][2], ls.wt[k]);   // w = the lobe weight c0..c3.w
      else set4(k, ls.hl_col[0], ls.hl_col[1], ls.hl_col[2], 1.f);              // [4] = the key light
    }
    return true;
  }
  if (name == "DifLghVec") {
    for (uint32_t k = 0; k < vec4s; ++k) {
      if (k < 4) set4(k, ls.dir[k][0], ls.dir[k][1], ls.dir[k][2], 0.f);
      else set4(k, key_world[0], key_world[1], key_world[2], 0.f);   // S103 L7: [4] = the key light, camera-attached
    }
    return true;
  }
  if (name == "DifLghMax") { set4(0, c.cube_scale[0], c.cube_scale[1], c.cube_scale[2], 1.f); return true; }
  if (name == "DifLghMaxA") { set4(0, c.cube_scale[3], 0.f, 0.f, 0.f); return true; }
  if (name == "LightLimit" || name == "fvLightLimit") { std::memcpy(out, mt.light_limit, 16); return true; }
  if (name == "SSS") { set4(0, mt.sss_col[0], mt.sss_col[1], mt.sss_col[2], mt.sss_sin_lvl); return true; }   // c155 = (g_f3SSSSinCol.xyz, g_fSSSSinLvl) (S111 UNIFY5: rows 0x8392A2E0 / 0x8392A300)
  if (name == "SSSSinCol2") { std::memcpy(out, mt.sss_col2, 16); return true; }
  // S111 MATFEED: c156 = (g_fHdLigLvl, g_fHdLigSt, 0, 0) [C: sub_82BAAEE8 rows 0x8392A320 / 0x8392A340, register 156 lanes x / y]; x = the body's live g_fHdLigLvl row when the record carries one
  // (CharCtx::hd_lig_lvl: the 'HRLG' record sub_826E1CA0 binds, its tick sub_826E1E20), else the file value; none named = (0, 1): no blow-out (the game keeps the last draw's c156: a 360 habit, not reproduced).
  if (name == "HdLig") { set4(0, c.hd_lig_lvl ? *c.hd_lig_lvl : mt.hd_lig[0], mt.hd_lig[1], 0.f, 0.f); return true; }
  if (name == "Reflect") { set4(0, mt.ref_col[0], mt.ref_col[1], mt.ref_col[2], mt.ref_col[3]); return true; }
  if (name == "SpecCubPrm1") { set4(0, mt.spec_lev, bake_pow(mt.spec_pow), mt.spec_lv_base, bake_pow(mt.spec_pw_base)); return true; }
  if (name == "SpecCubPrm3") { set4(0, mt.spec_col[0], mt.spec_col[1], mt.spec_col[2], mt.spec_col[3]); return true; }   // S111 SPECCOL: c144 = the material's g_f4SpecCol, the whole vec4 [C: sub_82BAAEE8 0x82BAC634..0x82BAC668]; none named = Material's white (the game keeps the last draw's c144: a 360 habit, not reproduced)
  // S103 B3F: the hair highlight constants [C: sub_82BAAEE8 0x82BAD42C-0x82BAD5A0]: c249 = (PrimaryShift, SecondShift, SpecExp1, SpecExp2), c250 = (SpeCol1.rgb, HairTileU), c251 = (SpeCol2.rgb, HairTileV).
  if (name == "HairSpeParam") { set4(0, mt.hair_shift[0], mt.hair_shift[1], mt.hair_exp[0], mt.hair_exp[1]); return true; }
  if (name == "SpeCol1") { set4(0, mt.hair_col1[0], mt.hair_col1[1], mt.hair_col1[2], mt.hair_tile[0]); return true; }
  if (name == "SpeCol2") { set4(0, mt.hair_col2[0], mt.hair_col2[1], mt.hair_col2[2], mt.hair_tile[1]); return true; }
  if (name == "NoiseTile") { set4(0, 50.f, mt.noise_near[0], mt.noise_near[1], mt.noise_near[2]); return true; }   // c206 = (50, bump, diffuse, light)
  if (name == "NoiseTileF") { set4(0, 50.f, mt.noise_far[0], mt.noise_far[1], mt.noise_far[2]); return true; }
  if (name == "View") { set4(0, 1.f, 0.f, 0.f, 0.f), set4(1, 0.f, 1.f, 0.f, 0.f), set4(2, 0.f, 0.f, 1.f, 0.f); return true; }  // (the reflection cube is black: the view matrix is not read)
  // S111 MATFEED: c249 = g_f4MuscleParam (whole vec4, row 0x8392B000) = the body's live row first (CharCtx::muscle: the muscle controller's record, sub_826E1010 relinks it per g_iMuscleId),
  // else the file value; c250 = (g_f4MuscleUV_x, _y, _z, _w) (rows 0x8392B020..0x8392B080, lanes x..w). None named = (1, 0, 0, 0) / (0, 0, 1, 1): the muscle map off.
  if (name == "MuscleParam") { if (c.muscle) set4(0, c.muscle[0], c.muscle[1], c.muscle[2], c.muscle[3]); else set4(0, mt.muscle_prm[0], mt.muscle_prm[1], mt.muscle_prm[2], mt.muscle_prm[3]); return true; }
  if (name == "MuscleUV") { set4(0, mt.muscle_uv[0], mt.muscle_uv[1], mt.muscle_uv[2], mt.muscle_uv[3]); return true; }
  // S103 WRK (WRK.md 1a [C]): PS c230 = the material's g_f4WrinkUV_* (a file value; an all-zero rect divides by zero in the shader exactly as the game's does: the level then decides); (0,0,1,1) only when the material names none.
  // A degenerate rectangle (z == x or w == y: the x4 face group's zeros) is fed (0,0,1,1) so the shader's 1 / (zw - xy) stays finite (review F2); with the switch off the old constant.
  if (name == "WrinkUV") {
    const bool live = assets::g_wrk_live.load(std::memory_order_relaxed);
    const bool deg = mt.wrink_uv[2] == mt.wrink_uv[0] || mt.wrink_uv[3] == mt.wrink_uv[1];
    if (live && !deg) set4(0, mt.wrink_uv[0], mt.wrink_uv[1], mt.wrink_uv[2], mt.wrink_uv[3]); else set4(0, 0.f, 0.f, 1.f, 1.f);
    return true;
  }
  // S103 WRK: PS c145 = g_f4IdMapCol (a whole vec4 file value); VS c237-c238 = g_fIdMapLv0..7, c231-c232 = g_fWklLv0..5 (c232.yzw unused): the LIVE per-wrestler levels of the record (null = 0 = no tint / no wrinkle depth).
  if (name == "IdMapCol") { if (assets::g_wrk_live.load(std::memory_order_relaxed)) set4(0, mt.idmap_col[0], mt.idmap_col[1], mt.idmap_col[2], mt.idmap_col[3]); return true; }
  if (name == "IdMapLvLst") { if (c.idmap) set4(0, c.idmap[0], c.idmap[1], c.idmap[2], c.idmap[3]), set4(1, c.idmap[4], c.idmap[5], c.idmap[6], c.idmap[7]); return true; }
  if (name == "WklLvLst") { if (c.wkl) set4(0, c.wkl[0], c.wkl[1], c.wkl[2], c.wkl[3]), set4(1, c.wkl[4], c.wkl[5], 0.f, 0.f); return true; }
  // S108 THR2: g_fChangeColor[0..15] = the Threads editor's colours (sub_8339F0E0: (R, G, B, 1) / 255), only on a change-colour draw (CharCtx::change_color); every other draw keeps zero.
  if (name == "ChangeColor") { if (c.change_color) std::memcpy(out, c.change_color, sizeof(float) * 4 * std::min<uint32_t>(vec4s, uint32_t(live::change_colour::kColours))); return true; }
  // S111 MATFEED: hair c243 = (g_fSSLevel, g_fSSStart, g_fSSEnd, 0) [C: rows 0x8392ADA0 / 0x8392ADC0 / 0x8392ADE0, register 243 lanes x / y / z], c244 = g_f4SSColor (whole vec4, row 0x8392AE00): file values; none named = zero (the glow off).
  if (name == "SS") { set4(0, mt.ss[0], mt.ss[1], mt.ss[2], 0.f); return true; }
  if (name == "SSColor") { set4(0, mt.ss_col[0], mt.ss_col[1], mt.ss_col[2], mt.ss_col[3]); return true; }
  // Zero is the neutral value: the option that reads these is not chosen, or the file carries no value (hair highlights: unconfirmed).
  if (name == "FogColor" || name == "Blood" || name == "BSSBldClr" || name == "Sweat" || name == "DebugPS0" || name == "DebugPS1" ||
      name == "ShadowPrm" || name == "ViewLightDepth" || name == "TexProjLightXY" || name == "TexProjLightW" || name == "ConvScreen")
    return true;
  return false;
}

}  // namespace rdr_scene
