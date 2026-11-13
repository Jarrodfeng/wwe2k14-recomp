// Session 101, lane 13: the scene front-end's character path. See char_effects.h and docs\renderer\SCENE_FRONTEND.md "Wrestlers".
#include "char_effects.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"

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
  return lc.find("aniso") != std::string::npos || lc.find("trans") != std::string::npos;
}

CharSlot RoleOfSlot(int slot) {
  switch (slot) {
    case 0: return CharSlot::Diffuse;
    case 1: return CharSlot::Normal;
    case 3: return CharSlot::NoiseTile;
    case 6: return CharSlot::Mask;
    case 10: return CharSlot::FlatBlackCube;
    case 13: return CharSlot::LightCube;
    case 15: return CharSlot::RimMap;
    case 5: case 8: case 9: return CharSlot::FlatGrey;
    default: return CharSlot::FlatBlack;  // 2, 4, 7, 14: team-colour masks and logo layers
  }
}

// ---- the choice ----------------------------------------------------------------------------------------------------------------

CharChoice ChooseCharEffect(const std::string& effect, bool skinned, bool alpha_test) {
  CharChoice best;
  best.effect = effect;
  double best_score = -1e9;
  bool any_vertex = false, any_fragment = false;
  for (int f = 0; f < kVariantCount; ++f) {
    const char* fn = kVariants[f].name;
    if (!fn || !T().by_index[size_t(f)] || !EndsWith(fn, "_fragment") || OwnerOf(fn) != effect) continue;
    any_fragment = true;
    const fxt::VariantTable& ft = *T().by_index[size_t(f)];
    const std::vector<std::string> fdef = Defines(ft);
    // Options with a stand-in that does nothing: all-zero team-colour masks and logo layers (black textures), a muscle / wrinkle map at its neutral value.
    if (!Subset(fdef, {"FX_CHANGE_COLOR", "FX_LOGO", "FX_MUSCLE", "FX_WRINKLES", "FX_ENV_LIGHT"})) continue;
    const rdr::fx::Variant& fv = kVariants[f];
    bool slots_ok = true;
    for (uint32_t s = 0; s < fv.sampler_count; ++s) {
      const int slot = fv.samplers[s].slot, kind = fv.samplers[s].kind;
      const bool known = slot == 0 || slot == 1 || slot == 2 || slot == 3 || slot == 4 || slot == 5 || slot == 6 || slot == 7 || slot == 8 || slot == 9 || slot == 10 || slot == 13 || slot == 14 || slot == 15;
      if (!known || (kind != 0 && !((slot == 13 || slot == 10) && kind == 1))) slots_ok = false;
    }
    if (!slots_ok) continue;
    if (alpha_test && !fv.spirv_alpha) continue;
    for (int v = 0; v < kVariantCount; ++v) {
      const char* vn = kVariants[v].name;
      if (!vn || !T().by_index[size_t(v)] || !EndsWith(vn, "_vertex") || OwnerOf(vn) != effect) continue;
      const fxt::VariantTable& vt = *T().by_index[size_t(v)];
      const std::vector<std::string> vdef = Defines(vt);
      if (!Subset(vdef, {"FX_SKINNED", "FX_VIEW_NORMAL"}) || Has(vdef, "FX_SKINNED") != skinned) continue;
      any_vertex = true;
      if (kVariants[v].sampler_count) continue;
      bool inputs_ok = true;
      for (uint32_t k = 0; k < vt.input_count; ++k) inputs_ok = inputs_ok && SupportedCharInput(vt.inputs[k].name);
      if (!inputs_ok) continue;
      bool match = true;
      int renamed = 0;
      for (uint32_t k = 0; k < ft.interp_count && match; ++k) {
        bool found = false;
        for (uint32_t j = 0; j < vt.interp_count; ++j) {
          if (vt.interps[j].location != ft.interps[k].location) continue;
          found = true;
          renamed += std::strcmp(vt.interps[j].name, ft.interps[k].name) != 0;
        }
        match = found;
      }
      if (!match) continue;
      const double score = -2.0 * renamed - 1.0 * double(fdef.size()) - 0.1 * double(vdef.size());
      if (score > best_score) {
        best_score = score;
        best.vs = v;
        best.ps = f;
      }
    }
  }
  if (!best.Ok()) {
    best.vs = best.ps = -1;
    best.why_not = !any_fragment ? "no effect '" + effect + "' in the registry yet"
                  : !any_vertex  ? "no " + std::string(skinned ? "skinned" : "rigid") + " vertex variant of '" + effect + "'"
                                 : "every variant of '" + effect + "' needs a runtime pass not fed yet (shadow map, cube reflection, blood, sweat, debug)";
  }
  return best;
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
      }
      cur->indices.push_back(it->second);
    }
  }
  return true;
}

// ---- constants by name ---------------------------------------------------------------------------------------------------------

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
  if (name == "Param0") { set4(0, mt.fairy, mt.fairy_edge, 0.f, 0.f); return true; }
  if (name == "RimLightDepth") { set4(0, 0.f, 1.f, 1.f, 0.f); return true; }
  if (name == "RimLightParam3") { set4(0, 500.f, 2500.f, 1.f, 0.4f); return true; }  // c174 = g_fRimAtn (500, 2500, 1, 0.4)
  if (name == "RimLightParam") { set4(0, 1.f, 0.83f, 1.f, 0.f); return true; }       // c161 = (1, 0.83, 1)
  if (name == "KeyLightColor") { set4(0, ls.hl_col[0], ls.hl_col[1], ls.hl_col[2], 1.f); return true; }
  if (name == "KeyLightVec") { set4(0, ls.hl_dir[0], ls.hl_dir[1], ls.hl_dir[2], 0.f); return true; }
  if (name == "Bools") {  // four bones: bits 1, 2, 3 of the first word (the vertex stage); bit 150 (logo) stays off
    const uint32_t bits = 0xEu;
    std::memcpy(out, &bits, 4);
    return true;
  }
  if (name == "NoiseLodParam") { set4(0, mt.noise_dist[0], mt.noise_dist[1], 0.f, 0.f); return true; }
  if (name == "Level") return true;  // the fixed mip of the colour map: 0
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
      else set4(k, ls.hl_dir[0], ls.hl_dir[1], ls.hl_dir[2], 0.f);
    }
    return true;
  }
  if (name == "DifLghMax") { set4(0, c.cube_scale[0], c.cube_scale[1], c.cube_scale[2], 1.f); return true; }
  if (name == "DifLghMaxA") { set4(0, c.cube_scale[3], 0.f, 0.f, 0.f); return true; }
  if (name == "LightLimit" || name == "fvLightLimit") { std::memcpy(out, mt.light_limit, 16); return true; }
  if (name == "SSS") { set4(0, mt.sss_col[0], mt.sss_col[1], mt.sss_col[2], mt.has_sss ? mt.sss_col[3] : 0.f); return true; }
  if (name == "SSSSinCol2") { std::memcpy(out, mt.sss_col2, 16); return true; }
  if (name == "HdLig") { set4(0, 0.f, 1.f, 0.f, 0.f); return true; }  // the blow-out knee is 0 in the game's draws (viewer_hlsl.h)
  if (name == "Reflect") { set4(0, mt.ref_col[0], mt.ref_col[1], mt.ref_col[2], mt.ref_col[3]); return true; }
  if (name == "SpecCubPrm1") { set4(0, mt.spec_lev, mt.spec_pow, mt.spec_lv_base, mt.spec_pw_base); return true; }
  if (name == "SpecCubPrm3") { set4(0, 1.f, 1.f, 1.f, 0.f); return true; }
  if (name == "NoiseTile") { set4(0, 50.f, mt.noise_near[0], mt.noise_near[1], mt.noise_near[2]); return true; }   // c206 = (50, bump, diffuse, light)
  if (name == "NoiseTileF") { set4(0, 50.f, mt.noise_far[0], mt.noise_far[1], mt.noise_far[2]); return true; }
  if (name == "View") { set4(0, 1.f, 0.f, 0.f, 0.f), set4(1, 0.f, 1.f, 0.f, 0.f), set4(2, 0.f, 0.f, 1.f, 0.f); return true; }  // (the reflection cube is black: the view matrix is not read)
  if (name == "MuscleParam") { set4(0, 1.f, 0.f, 0.f, 0.f); return true; }
  if (name == "MuscleUV" || name == "WrinkUV") { set4(0, 0.f, 0.f, 1.f, 1.f); return true; }
  // Zero is the neutral value: the option that reads these is not chosen, or the file carries no value (hair highlights: unconfirmed).
  if (name == "FogColor" || name == "ChangeColor" || name == "Blood" || name == "BSSBldClr" || name == "Sweat" || name == "DebugPS0" || name == "DebugPS1" ||
      name == "ShadowPrm" || name == "ViewLightDepth" || name == "Param1" || name == "TexProjLightXY" || name == "TexProjLightW" || name == "ConvScreen" ||
      name == "HairSpeParam" || name == "SpeCol1" || name == "SpeCol2" || name == "SSColor" || name == "SS" || name == "SphereOccP" || name == "SphereOccV" ||
      name == "IdMapCol" || name == "IdMapLvLst" || name == "WklLvLst")
    return true;
  return false;
}

}  // namespace rdr_scene
