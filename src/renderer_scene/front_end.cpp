// The scene front-end: scene record tables -> renderer frames. See front_end.h and docs\renderer\SCENE_FRONTEND.md.
#include "front_end.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"
#include "shader_reload.h"  // S101 lane24

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
constexpr int kVariantCount = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));

// ---- the effect tables (names -> registry index) ------------------------------------------------------------------------------

struct Tables {
  std::vector<const fxt::VariantTable*> by_index;                 // parallel to rdr::fx::kVariants
  std::unordered_map<std::string, int> index_of;                  // variant name -> registry index
  Tables() {
    std::unordered_map<std::string, const fxt::VariantTable*> named;
    for (const fxt::VariantTable& t : fxt::kTables) named.emplace(t.variant, &t);
    by_index.assign(size_t(kVariantCount), nullptr);
    for (int i = 0; i < kVariantCount; ++i) {
      if (!kVariants[i].name) continue;
      index_of.emplace(kVariants[i].name, i);
      const auto it = named.find(kVariants[i].name);
      if (it != named.end()) by_index[size_t(i)] = it->second;
    }
  }
};
const Tables& T() {
  static const Tables tables;
  return tables;
}

bool HasDefine(const fxt::VariantTable& t, const char* define) {
  const std::string all = std::string(" ") + (t.defines ? t.defines : "") + " ";
  return all.find(std::string(" ") + define + " ") != std::string::npos;
}

// Options the front-end cannot feed yet (they need a shadow map, a cube map, a second texture layer, a bone palette, a logo ...): a variant
// with one of them is not chosen. The list is the honest edge of version 1 (docs\renderer\SCENE_FRONTEND.md).
bool Unsupported(const fxt::VariantTable& t, bool allow_skinned = false, bool allow_reflect = false) {  // S101 lane 15: allow_skinned lets FX_SKINNED through (a skinned prop)
  static const char* const kBlocked[] = {"FX_SHADOW", "FX_SHADOW_LIGHT", "FX_SCREEN_SHADOW", "FX_LIGHT_SHADOW", "FX_REFLECT", "FX_REFLECT_ADD", "FX_REFLECT_MASK",
                                         "FX_REFLECT_MAP", "FX_CONV_SCREEN", "FX_SCREEN_CONVERT", "FX_EDGE_FADE", "FX_NOISE", "FX_OVERLAY", "FX_LOGO",
                                         "FX_PICKUP", "FX_CAA", "FX_CC_MASK", "FX_SKINNED", "FX_VELOCITY", "VELOCITY", "FX_VERTEX_LM", "FX_SPEC_FADE",
                                         "FX_PLANAR_UV", "FX_PLANAR_YZ", "FX_LAYER2", "FX_LAYER3", "FX_LAYER4", "FX_UV2", "FX_UV3", "FX_UV4",
                                         "FX_LM_VERTEX", "FX_FRAME", "FX_EYE"};
  for (const char* d : kBlocked)
    if (HasDefine(t, d) && !(allow_skinned && std::strcmp(d, "FX_SKINNED") == 0) && !(allow_reflect && std::strcmp(d, "FX_REFLECT") == 0)) return true;  // S101 lane 15: FX_REFLECT with the black cube (no reflection layer)
  return false;
}

// S101 lane 15: a skinned yProp_* piece (a belt, a held prop: the viewer poses it with a bone palette) is cut into skin chunks like a character piece.
bool IsSkinnableProp(const std::string& effect) { return effect.rfind("yProp_", 0) == 0; }

// S101 lane 15: the materials the game draws with their light-map ("LM") variant in an entrance (viewer renderer.cpp GpuMaterial::ent_lm; the yBG_* families are off by default).
bool EntranceLmClass(const assets::Material& m, bool bg) {
  if (m.crowd || m.lightmap >= 0) return false;
  std::string lc = m.shader;
  for (char& ch : lc) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return lc == "yprop_default" || lc == "yprop_psm" || (bg && (lc == "ybg_default" || lc == "ybg_uvscroll"));  // (the viewer's ent_lm_bg switch)
}
bool IsMovieSurface(const std::string& part) {  // the stage movie surfaces keep their unlit look (no entrance LM)
  int key = 0;
  return std::sscanf(part.c_str(), "slot %d", &key) == 1 && key >= 800 && key <= 809;
}

bool SupportedInput(const char* name, bool skinned = false) {
  static const char* const kOk[] = {"inPosition", "inNormal", "inColor", "inUv", "inTexCoord", "inUv2", "inTangent", "inBoneId"};
  if (skinned && (std::strcmp(name, "inWeights") == 0 || std::strcmp(name, "inBoneIds") == 0)) return true;  // S101 lane 15: the second stream of a skinned prop chunk
  for (const char* n : kOk)
    if (std::strcmp(name, n) == 0) return true;
  return false;
}

std::string LmTwin(const std::string& effect) {
  if (effect == "yProp_UVScroll") return "yProp_UVScrolLM";  // (sic: the effect's own spelling)
  return effect + "LM";
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void Collect(const std::string& effect, std::vector<int>& vertex, std::vector<int>& fragment) {
  const std::string prefix = effect + "_";
  for (int i = 0; i < kVariantCount; ++i) {
    const char* n = kVariants[i].name;
    if (!n || !T().by_index[size_t(i)]) continue;
    const std::string name = n;
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    if (EndsWith(name, "_vertex")) vertex.push_back(i);
    else if (EndsWith(name, "_fragment")) fragment.push_back(i);
  }
}

}  // namespace

// ---- ChooseEffect -------------------------------------------------------------------------------------------------------------

FxChoice ChooseEffect(const std::string& effect, const MaterialWants& w) {
  FxChoice best;
  best.effect = effect;
  double best_score = -1e9;
  std::string why;
  std::vector<std::string> candidates;
  if (w.lightmap) candidates.push_back(LmTwin(effect));
  candidates.push_back(effect);
  // S101 lane 15: the Reflect effects (yBG_Reflect, yProp_Reflect: a vehicle's paint, chrome) read a cube map at slot 8 for their reflection layer. The cube is not fed
  // (it is the stage environment the game bakes at run time): a black cube plus ReflectAlpha = 0 (Resolve: zero) is the do-nothing value, so the surface keeps its diffuse +
  // highlight and has no reflection layer. It is neutral ONLY because ReflectAlpha is 0: FX_REFLECT blends (fx_prop.glsl, fx_bg.glsl), so feeding ReflectAlpha later would
  // darken these surfaces. Counted as a stand-in in the coverage line. Only the plain FX_REFLECT option is taken; the add / map / mask options stay unbound.
  const bool allow_reflect = effect.find("Reflect") != std::string::npos;
  for (const std::string& cand : candidates) {
    std::vector<int> vertex, fragment;
    Collect(cand, vertex, fragment);
    if (vertex.empty() || fragment.empty()) {
      why = "no effect '" + cand + "' in the registry yet";
      continue;
    }
    const bool is_lm_cand = cand != effect;
    std::string cand_why = "every variant of '" + cand + "' needs something not fed yet (shadow, cube, second layer, bones ...)";
    for (int f : fragment) {
      const fxt::VariantTable& ft = *T().by_index[size_t(f)];
      if (Unsupported(ft, false, allow_reflect)) continue;
      const rdr::fx::Variant& fv = kVariants[f];
      bool slots_ok = true, reads_lm = false;
      for (uint32_t s = 0; s < fv.sampler_count; ++s) {
        const int slot = fv.samplers[s].slot;
        if (slot == 13) { reads_lm = true; slots_ok = slots_ok && w.lightmap; }
        else if (slot == 0 || slot == 6) {}
        else if (slot == 4) slots_ok = slots_ok && w.normal;
        else if (slot == 5) slots_ok = slots_ok && w.specmap;
        else if (slot == 8 && allow_reflect && fv.samplers[s].kind == 1) {}  // the black reflection cube
        else slots_ok = false;
        if (fv.samplers[s].kind != 0 && !(slot == 8 && allow_reflect && fv.samplers[s].kind == 1)) slots_ok = false;
      }
      if (!slots_ok) continue;
      if (w.alpha_test && !fv.spirv_alpha) continue;
      for (int v : vertex) {
        const fxt::VariantTable& vt = *T().by_index[size_t(v)];
        if (Unsupported(vt, w.skinned, allow_reflect)) continue;
        if (HasDefine(vt, "FX_SKINNED") != w.skinned) continue;  // S101 lane 15: a skinned prop takes the skinned vertex variant, a rigid one the plain one
        bool inputs_ok = true;
        for (uint32_t k = 0; k < vt.input_count; ++k) inputs_ok = inputs_ok && SupportedInput(vt.inputs[k].name, w.skinned);
        if (!inputs_ok) continue;
        if (kVariants[v].sampler_count) continue;  // a vertex texture (the per-vertex light map)
        // Every interpolator the fragment reads must be written by the vertex stage at the same location. The authors named the same
        // slot differently now and then (Normal / NegNormal), so the location decides and an equal name only scores.
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
        double score = -2.0 * renamed;
        const auto count = [](const fxt::VariantTable& t) {
          int n = 0;
          for (const char* p = t.defines; p && *p; ++p) n += *p == ' ';
          return (t.defines && *t.defines) ? n + 1 : 0;
        };
        score -= 0.1 * (count(ft) + count(vt));
        const bool has_spec = HasDefine(ft, "FX_SPECULAR") || HasDefine(ft, "FX_SPEC") || HasDefine(vt, "FX_SPECULAR") || HasDefine(vt, "FX_SPEC");
        score += has_spec ? (w.spec ? 4 : -4) : (w.spec ? -1 : 0);
        if (HasDefine(ft, "FX_SPEC_MAP") && w.specmap) score += 2;
        // S101 lane 257: an arena / prop material with a highlight and no specular map keeps its gloss mask in the diffuse alpha (the viewer rule, viewer_hlsl.h "arena highlight:
        // strength in the diffuse alpha"; jboy.h: floor / stair / rope mats have alpha 60-110 everywhere, a mask, not a cut-out). The FX_ALPHA_SPEC pair multiplies the highlight by it.
        // Without this the plain FX_SPECULAR pair was chosen: a full-strength lobe (stand floors: level 0.5, power 5) that washed floors and curtains white (census tile faults).
        if (HasDefine(ft, "FX_ALPHA_SPEC")) score += (w.spec && !w.specmap && !w.alpha_test) ? 3.0 : -3.0;
        if (w.lightmap && !reads_lm) score -= 50;
        if (!w.lightmap && reads_lm) continue;
        (void)is_lm_cand;
        if (score > best_score) {
          best_score = score;
          best.vs = v;
          best.ps = f;
          best.effect = cand;
        }
      }
    }
    if (best.vs < 0) why = cand_why;
  }
  if (!best.Ok()) {
    best.vs = best.ps = -1;
    best.effect = effect;
    best.why_not = why.empty() ? "no variant" : why;
  }
  return best;
}

// ---- per-model data -----------------------------------------------------------------------------------------------------------

struct FrontEnd::Bound {
  FxChoice choice;
  bool debug = false;
  uint16_t program = 0, layout = 0;
  rdr::ImageId slot_image[rdr::kMaxTextures] = {};
  uint8_t slot_clamp[rdr::kMaxTextures] = {};
  bool alpha_test = false;
  bool chr = false, hair = false;                 // S101 lane 13: a yCh_* material (the character path); hair / translucent: drawn twice
  uint8_t cube_slot[rdr::kMaxTextures] = {};      // 1 = this sampler slot holds the object's ambient cube (13), 2 = a screen-space map (clamped)
};

struct FrontEnd::ModelData {
  std::string label;
  std::vector<rdr::ImageId> images;
  struct ImgInfo { uint32_t w = 0, h = 0, mips = 0; bool rgba8 = false; };  // S101 lane 15: what UpdateTexture may replace
  std::vector<ImgInfo> img_info;
  struct Mesh {
    uint32_t vb_offset = 0, ib_offset = 0, index_count = 0, vertex_count = 0;
    int material = -1, placement = -1, light_set = -1;
    bool skinned = false;
    float center[3] = {0, 0, 0};
    std::string part;
    int src = 0;                      // S101 lane 13: the ordinal of the source mesh (Object::mesh_mask bits)
    bool cloth = false;               // a skinned cloth piece: posed on the CPU every frame (the effect does not skin its position)
    std::vector<assets::Vertex> cpu;  // ... its bind vertices
    bool chunk = false;               // a skin chunk of a character mesh: the second vertex stream below and `nodes` are valid
    uint32_t sv_offset = 0;           // into ModelData::svb
    std::vector<uint16_t> nodes;      // palette slot -> skeleton node
  };
  std::vector<Mesh> meshes;
  std::vector<assets::Material> materials;
  std::vector<std::vector<assets::Matrix>> placements;
  std::vector<assets::LightSet> light_sets;
  int stage_lm = -1;
  float lm_xy[2] = {1.f / 3500.f, 1.f / 3500.f}, lm_gain = 2.5f;
  std::unordered_map<uint64_t, rdr::BufferId> cloth_bufs;  // posed cloth vertices, by (object, mesh)
  rdr::BufferId vb = rdr::kNoBuffer, ib = rdr::kNoBuffer, svb = rdr::kNoBuffer;  // (svb: the skin stream of the character chunks)
  std::vector<std::unique_ptr<Bound>> bound;  // per material (x2: rigid / skinned, S101 lane 13)
  bool live = true;
};

FrontEnd::FrontEnd(rdr::Renderer& renderer, const FrontEndOptions& options) : renderer_(renderer), options_(options) {
  const auto flat = [&](uint8_t v) {
    rdr::ImageDesc d;
    d.width = d.height = 1;
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    const rdr::ImageId id = renderer_.CreateImage(d);
    const uint8_t px[4] = {v, v, v, 255};
    renderer_.UpdateImage(id, 0, 0, px, 4);
    return id;
  };
  white_ = flat(255);
  grey_ = flat(128);
  {  // S101 lane 13: an all-zero layer (team-colour masks, logos, the rim map) and a normal map with no bump and half gloss
    rdr::ImageDesc d;
    d.width = d.height = 1;
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    const uint8_t zero[4] = {0, 0, 0, 0}, neutral[4] = {128, 128, 128, 128};
    black_ = renderer_.CreateImage(d);
    renderer_.UpdateImage(black_, 0, 0, zero, 4);
    neutral_normal_ = renderer_.CreateImage(d);
    renderer_.UpdateImage(neutral_normal_, 0, 0, neutral, 4);
    rdr::ImageDesc cd = d;  // a black cube (the reflection layer of yCh_Reflect: the stage's environment is not fed)
    cd.layers = 6;
    cd.kind = rdr::ImageKind::Cube;
    black_cube_ = renderer_.CreateImage(cd);
    for (uint32_t face = 0; face < 6; ++face) renderer_.UpdateImage(black_cube_, 0, face, zero, 4);
  }
  const auto& idx = T().index_of;
  const auto vs = idx.find("yDebugPrimitive_base_vertex"), ps = idx.find("yDebugPrimitive_base_fragment");
  if (vs != idx.end() && ps != idx.end()) debug_vs_ = vs->second, debug_ps_ = ps->second;
}

FrontEnd::~FrontEnd() {
  renderer_.WaitIdle();
  for (size_t i = 0; i < models_.size(); ++i) ReleaseModel(int(i));
  for (ModelData* m : models_) delete m;
  post_pass_.reset();  // S101 lane 34 (its images go first)
  for (rdr::ImageId id : {white_, grey_, color_, depth_, black_, neutral_normal_, black_cube_, post_out_})
    if (id != rdr::kNoImage) renderer_.DestroyImage(id);
  for (auto& kv : cubes_)
    if (kv.second.image != rdr::kNoImage) renderer_.DestroyImage(kv.second.image);
}

FrontEnd::ModelData& FrontEnd::Mod(int id) { return *models_[size_t(id)]; }

void FrontEnd::ReleaseModel(int id) {
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return;
  ModelData& m = Mod(id);
  m.live = false;
  for (rdr::ImageId image : m.images)
    if (image != rdr::kNoImage) renderer_.DestroyImage(image);
  m.images.clear();
  if (m.vb != rdr::kNoBuffer) renderer_.DestroyBuffer(m.vb);
  if (m.ib != rdr::kNoBuffer) renderer_.DestroyBuffer(m.ib);
  if (m.svb != rdr::kNoBuffer) renderer_.DestroyBuffer(m.svb);
  m.vb = m.ib = m.svb = rdr::kNoBuffer;
  for (auto& kv : m.cloth_bufs)
    if (kv.second != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second);
  m.cloth_bufs.clear();
}

// ---- textures -----------------------------------------------------------------------------------------------------------------

static size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

rdr::ImageId FrontEnd::MakeTexture(const assets::Texture& t, std::string& warning) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) { warning = "empty or unknown format"; return rdr::kNoImage; }
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if (bc && ((t.width % 4) || (t.height % 4))) { warning = "block-compressed size not a multiple of 4"; return rdr::kNoImage; }
  if (t.width > 16384 || t.height > 16384) { warning = "too large"; return rdr::kNoImage; }
  uint32_t mips = std::max(1u, t.mips), mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  mips = std::min(mips, maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < mips; ++i) {
    const size_t sz = MipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  if (fit == 0) { warning = "data shorter than mip 0"; return rdr::kNoImage; }
  rdr::ImageDesc d;
  d.width = uint16_t(t.width);
  d.height = uint16_t(t.height);
  d.mips = uint8_t(fit);
  d.usage = rdr::kUsageSampled;
  switch (t.format) {
    case assets::TexFormat::kBC1: d.format = rdr::Format::BC1; break;
    case assets::TexFormat::kBC2: d.format = rdr::Format::BC2; break;
    case assets::TexFormat::kBC3: d.format = rdr::Format::BC3; break;
    case assets::TexFormat::kB8G8R8A8: d.format = rdr::Format::BGRA8; break;
    default: d.format = rdr::Format::RGBA8; break;  // R8G8B8A8, and B4G4R4A4 widened below
  }
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) { warning = "CreateImage failed"; return rdr::kNoImage; }
  size_t src = 0;
  std::vector<uint8_t> wide;
  for (uint32_t i = 0; i < fit; ++i) {
    const uint32_t mw = std::max(1u, t.width >> i), mh = std::max(1u, t.height >> i);
    const size_t sb = MipBytes(t.format, mw, mh);
    if (t.format == assets::TexFormat::kB4G4R4A4) {
      wide.resize(size_t(mw) * mh * 4);
      for (size_t k = 0; k < size_t(mw) * mh; ++k) {
        const uint16_t px = uint16_t(t.data[src + 2 * k] | (t.data[src + 2 * k + 1] << 8));
        wide[4 * k] = uint8_t(((px >> 8) & 15) * 17), wide[4 * k + 1] = uint8_t(((px >> 4) & 15) * 17);
        wide[4 * k + 2] = uint8_t((px & 15) * 17), wide[4 * k + 3] = uint8_t(((px >> 12) & 15) * 17);
      }
      renderer_.UpdateImage(id, i, 0, wide.data(), wide.size());
    } else {
      renderer_.UpdateImage(id, i, 0, t.data.data() + src, sb);
    }
    src += sb;
  }
  return id;
}

// ---- models -------------------------------------------------------------------------------------------------------------------

int FrontEnd::AddModel(const assets::Model& model, std::vector<std::string>& warnings) {
  auto* m = new ModelData();
  m->label = model.label;
  for (const assets::Texture& t : model.textures) {
    std::string w;
    const rdr::ImageId id = MakeTexture(t, w);
    if (id == rdr::kNoImage) warnings.push_back("texture '" + t.name + "': " + w);
    if (static const bool dump_tex = std::getenv("WWE_RDR_DUMP_MATS") != nullptr; dump_tex) std::fprintf(stderr, "[texs] %-16s format=%d %ux%u mips=%u\n", t.name.c_str(), int(t.format), t.width, t.height, t.mips);  // lane 257 (dev)
    m->images.push_back(id);
    m->img_info.push_back({t.width, t.height, std::max(1u, t.mips), t.format == assets::TexFormat::kR8G8B8A8});
  }
  m->materials = model.materials;
  for (const assets::Placement& p : model.placements) m->placements.push_back(p.transforms);
  m->light_sets = model.light_sets;
  m->stage_lm = (model.stage_lightmap >= 0 && size_t(model.stage_lightmap) < m->images.size() && m->images[size_t(model.stage_lightmap)] != rdr::kNoImage)
                    ? model.stage_lightmap : -1;
  m->lm_xy[0] = model.lm_scale[0], m->lm_xy[1] = model.lm_scale[1], m->lm_gain = model.lm_gain;
  std::vector<uint8_t> vb, ib, svb;
  int src_index = -1;
  for (const assets::Mesh& me : model.meshes) {
    if (me.vertices.empty() || me.indices.size() < 3) continue;
    bool bad = false;
    for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
    if (bad) { warnings.push_back("mesh '" + me.part + "': index out of range, skipped"); continue; }
    if (me.part.rfind("crowd flash", 0) == 0) continue;  // the flash sprite is the viewer's flash pass, not a scene mesh
    ++src_index;
    const int material = (me.material >= 0 && size_t(me.material) < m->materials.size()) ? me.material : -1;
    // Appends one drawable piece (the whole mesh, or a skin chunk of it) to the model's buffers.
    const auto add_piece = [&](const std::vector<assets::Vertex>& verts, const std::vector<uint32_t>& indices, const SkinChunk* chunk) {
      ModelData::Mesh mm;
      mm.vb_offset = uint32_t((vb.size() + 15) & ~size_t(15));
      vb.resize(mm.vb_offset);
      vb.insert(vb.end(), reinterpret_cast<const uint8_t*>(verts.data()), reinterpret_cast<const uint8_t*>(verts.data() + verts.size()));
      mm.ib_offset = uint32_t(ib.size());
      ib.insert(ib.end(), reinterpret_cast<const uint8_t*>(indices.data()), reinterpret_cast<const uint8_t*>(indices.data() + indices.size()));
      mm.index_count = uint32_t(indices.size());
      mm.vertex_count = uint32_t(verts.size());
      mm.material = material;
      mm.placement = (me.placement >= 0 && size_t(me.placement) < m->placements.size()) ? me.placement : -1;
      mm.light_set = (me.light_set >= 0 && size_t(me.light_set) < m->light_sets.size()) ? me.light_set : -1;
      mm.skinned = me.skinned;
      mm.part = me.part;
      mm.src = src_index;
      if (chunk) {
        mm.chunk = true;
        mm.nodes = chunk->nodes;
        if (IsClothEffect(m->materials[size_t(material)].shader)) mm.cloth = true, mm.cpu = chunk->vertices;
        else if (!IsCharEffect(m->materials[size_t(material)].shader)) mm.cpu = chunk->vertices;  // S101 lane 15: a skinned prop chunk with no effect yet is posed on the CPU for its debug-colour draw
        mm.sv_offset = uint32_t((svb.size() + 15) & ~size_t(15));
        svb.resize(mm.sv_offset);
        svb.insert(svb.end(), reinterpret_cast<const uint8_t*>(chunk->skin.data()), reinterpret_cast<const uint8_t*>(chunk->skin.data() + chunk->skin.size()));
      }
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      for (const assets::Vertex& v : verts)
        for (int k = 0; k < 3; ++k)
          if (std::isfinite(v.pos[k])) mn[k] = std::min(mn[k], v.pos[k]), mx[k] = std::max(mx[k], v.pos[k]);
      for (int k = 0; k < 3; ++k) mm.center[k] = mn[k] <= mx[k] ? (mn[k] + mx[k]) * 0.5f : 0.f;
      m->meshes.push_back(mm);
    };
    if (me.skinned && material >= 0 && (IsCharEffect(m->materials[size_t(material)].shader) || IsSkinnableProp(m->materials[size_t(material)].shader))) {
      // S101 lane 13: a character piece is cut into chunks of at most 21 bones (the effects hold 21 bone matrices), each with its own bone stream.
      std::vector<SkinChunk> chunks;
      if (!BuildSkinChunks(me, chunks)) { warnings.push_back("mesh '" + me.part + "': a triangle needs more than 21 bones, skipped"); continue; }
      for (const SkinChunk& c : chunks) add_piece(c.vertices, c.indices, &c);
    } else {
      add_piece(me.vertices, me.indices, nullptr);
    }
  }
  if (!vb.empty()) m->vb = renderer_.CreateBuffer(vb.size(), vb.data());
  if (!ib.empty()) m->ib = renderer_.CreateBuffer(ib.size(), ib.data());
  if (!svb.empty()) m->svb = renderer_.CreateBuffer(svb.size(), svb.data());
  m->bound.resize((m->materials.size() + 1) * 4);  // x2 rigid / skinned, x2 entrance light map (S101 lane 15)  // x2: rigid / skinned
  models_.push_back(m);
  return int(models_.size()) - 1;
}

// ---- binding a material to an effect -----------------------------------------------------------------------------------------

uint16_t FrontEnd::ProgramFor(int vs, int ps, bool alpha) {
  const uint64_t key = uint64_t(uint32_t(vs)) << 32 | uint64_t(uint32_t(ps + 1)) << 1 | uint64_t(alpha);
  const auto found = programs_.find(key);
  if (found != programs_.end()) return found->second;
  const rdr::fx::Variant& vertex = kVariants[vs];
  rdr::ProgramDesc desc;
  names_.push_back(std::string(vertex.name) + " + " + kVariants[ps].name);
  desc.name = names_.back().c_str();
  desc.vertex_spirv = vertex.spirv;
  desc.vertex_bytes = vertex.bytes;
  const rdr::fx::Variant& fragment = kVariants[ps];
  const ShaderReload* reload = ShaderReload::Get();  // S101 lane24
  const bool twin = alpha && fragment.spirv_alpha && !(reload && reload->DropsTwin(ps));  // (a reload that dropped the twin: "alpha_test": false)
  desc.fragment_spirv = twin ? fragment.spirv_alpha : fragment.spirv;
  desc.fragment_bytes = twin ? fragment.bytes_alpha : fragment.bytes;
  uint16_t id = 0;
  bool overridden = false;
  if (reload) {  // S101 lane24: a variant's SPIR-V from the override folder / the watcher
    if (const std::vector<uint32_t>* w = reload->Find(vs, false)) desc.vertex_spirv = w->data(), desc.vertex_bytes = w->size() * 4, overridden = true;
    if (const std::vector<uint32_t>* w = reload->Find(ps, twin)) desc.fragment_spirv = w->data(), desc.fragment_bytes = w->size() * 4, overridden = true;
  }
  // A reloaded module is checked (the driver may refuse it; the id table is 16 bit and never shrinks): the compiled program stands in, said once per program.
  if (!overridden || !renderer_.AddProgramChecked(desc, id)) {
    if (overridden) std::fprintf(stderr, "[reload] %s: the reloaded shader was refused (driver or program table): the compiled one is used\n", desc.name);
    desc.vertex_spirv = vertex.spirv, desc.vertex_bytes = vertex.bytes;
    desc.fragment_spirv = twin ? fragment.spirv_alpha : fragment.spirv, desc.fragment_bytes = twin ? fragment.bytes_alpha : fragment.bytes;
    id = renderer_.AddProgram(desc);
  }
  programs_.emplace(key, id);
  return id;
}

// The vertex layout of an effect's vertex shader over assets::Vertex (one interleaved buffer: the file's streams as floats, the colour as
// RGBA8; the GPU converts). Each shader input is found by NAME.
uint16_t FrontEnd::LayoutFor(int vs) {
  const auto found = layouts_.find(vs);
  if (found != layouts_.end()) return found->second;
  constexpr uint16_t kRGB32F = 106, kRG32F = 103, kRGBA8Unorm = 37;
  rdr::VertexLayoutDesc desc{};
  desc.strides[0] = uint32_t(sizeof(assets::Vertex));
  desc.strides[1] = uint32_t(sizeof(SkinVertex));  // S101 lane 13: the second stream (palette slots as floats + three weights) of skinned character chunks
  const fxt::VariantTable& t = *T().by_index[size_t(vs)];
  for (uint32_t k = 0; k < t.input_count && desc.attribute_count < rdr::kMaxVertexAttributes; ++k) {
    const std::string name = t.inputs[k].name;
    rdr::VertexAttribute a{};
    a.location = t.inputs[k].location;
    if (name == "inPosition") a.format = kRGB32F, a.offset = uint32_t(offsetof(assets::Vertex, pos));
    else if (name == "inNormal") a.format = kRGB32F, a.offset = uint32_t(offsetof(assets::Vertex, normal));
    else if (name == "inColor") a.format = kRGBA8Unorm, a.offset = uint32_t(offsetof(assets::Vertex, color));
    else if (name == "inUv" || name == "inTexCoord") a.format = kRG32F, a.offset = uint32_t(offsetof(assets::Vertex, uv0));
    else if (name == "inUv2") a.format = kRG32F, a.offset = uint32_t(offsetof(assets::Vertex, uv1));
    else if (name == "inTangent") a.format = kRGB32F, a.offset = uint32_t(offsetof(assets::Vertex, tangent));
    else if (name == "inBoneId") a.format = 74 /* R16_UINT */, a.offset = uint32_t(offsetof(assets::Vertex, bones));  // a rigid draw: the first slot
    else if (name == "inBoneIds") a.binding = 1, a.format = 107 /* R32G32B32A32_UINT: the effects declare bone ids integer (compile_shaders.py as_integer) */, a.offset = uint32_t(offsetof(SkinVertex, ids));      // S101 lane 13
    else if (name == "inWeights") a.binding = 1, a.format = 106 /* R32G32B32_SFLOAT */, a.offset = uint32_t(offsetof(SkinVertex, w));
    else if (name == "inExtra") a.format = kRGBA8Unorm, a.offset = uint32_t(offsetof(assets::Vertex, color));  // the skin's x = the vertex colour's grey (jboy.h: the lobe gain), w = its alpha
    else continue;
    desc.attributes[desc.attribute_count++] = a;
  }
  const uint16_t id = renderer_.AddVertexLayout(desc);
  layouts_.emplace(vs, id);
  return id;
}

const FrontEnd::Bound& FrontEnd::BindMaterial(ModelData& m, int material, bool skinned, bool ent_lm) {
  const size_t slot = ((material >= 0 ? size_t(material) : m.materials.size()) * 2 + (skinned ? 1 : 0)) * 2 + (ent_lm ? 1 : 0);  // S101 lane 15: x2 entrance light map
  if (m.bound[slot]) return *m.bound[slot];
  auto b = std::make_unique<Bound>();
  assets::Material fallback;
  fallback.shader = "(no material)";
  const assets::Material& mat = material >= 0 ? m.materials[size_t(material)] : fallback;
  const auto tex = [&](int i) -> rdr::ImageId {
    return (i >= 0 && size_t(i) < m.images.size()) ? m.images[size_t(i)] : rdr::kNoImage;
  };
  if (IsCharEffect(mat.shader)) {  // S101 lane 13: the wrestlers' effects
    b->chr = true;
    b->hair = IsHairEffect(mat.shader);
    b->alpha_test = mat.alpha_test;
    const CharChoice cc = ChooseCharEffect(mat.shader, skinned, mat.alpha_test);
    b->choice.vs = cc.vs, b->choice.ps = cc.ps, b->choice.effect = mat.shader, b->choice.why_not = cc.why_not;
    EffectCoverage& cov = coverage_[mat.shader];
    cov.effect = mat.shader;
    if (cc.Ok()) {
      const std::string pair = std::string(kVariants[cc.vs].name) + " + " + kVariants[cc.ps].name;
      if (cov.bound_to.empty()) cov.bound_to = pair;
      b->program = ProgramFor(cc.vs, cc.ps, mat.alpha_test);
      b->layout = LayoutFor(cc.vs);
      const rdr::fx::Variant& fv = kVariants[cc.ps];
      for (uint32_t s = 0; s < fv.sampler_count; ++s) {
        const int slotn = fv.samplers[s].slot;
        rdr::ImageId image = white_;
        switch (RoleOfSlot(slotn)) {
          case CharSlot::Diffuse: image = tex(mat.diffuse) != rdr::kNoImage ? tex(mat.diffuse) : white_; break;
          case CharSlot::Normal: image = tex(mat.normal) != rdr::kNoImage ? tex(mat.normal) : (b->hair ? grey_ : neutral_normal_); break;
          case CharSlot::NoiseTile: image = tex(mat.detail) != rdr::kNoImage ? tex(mat.detail) : grey_; break;
          case CharSlot::Mask: image = tex(mat.mask) != rdr::kNoImage ? tex(mat.mask) : black_; break;
          case CharSlot::LightCube: b->cube_slot[slotn] = 1; break;
          case CharSlot::RimMap: image = black_, b->cube_slot[slotn] = 2; break;
          case CharSlot::FlatGrey: image = grey_; break;
          case CharSlot::FlatBlack: image = black_; break;
          case CharSlot::FlatBlackCube: image = black_cube_; b->cube_slot[slotn] = 3; break;
        }
        if (!options_.textures && image != black_ && !b->cube_slot[slotn]) image = grey_;  // the planted fault: no textures
        b->slot_image[slotn] = image == rdr::kNoImage ? white_ : image;
      }
    } else {
      b->debug = true;
      if (cov.reason.empty()) cov.reason = cc.why_not;
      if (debug_vs_ >= 0) {
        b->program = ProgramFor(debug_vs_, debug_ps_, false);
        b->layout = LayoutFor(debug_vs_);
      }
    }
    m.bound[slot] = std::move(b);
    return *m.bound[slot];
  }
  MaterialWants w;
  w.diffuse = tex(mat.diffuse) != rdr::kNoImage;
  const rdr::ImageId stage = m.stage_lm >= 0 ? tex(m.stage_lm) : rdr::kNoImage;
  const rdr::ImageId own_lm = tex(mat.lightmap);
  // A mesh with its own light map or the stage's slot takes the stage-wide top-down map: the effects derive the light-map uv from the
  // world position (the game's rule), so the mesh's own second-uv map cannot be fed (named in the page; the entrance rule `ent_own_lm`).
  const rdr::ImageId lm_image = stage != rdr::kNoImage ? stage : own_lm;
  w.lightmap = (own_lm != rdr::kNoImage || mat.has_lm_slot || ent_lm) && lm_image != rdr::kNoImage;  // S101 lane 15: ent_lm = the entrance draws this class with its LM variant
  w.skinned = skinned;
  w.normal = tex(mat.normal) != rdr::kNoImage;
  // S101 lane 257: a normal-mapped class (yBG_BumpMap, yProp_BumpMap) whose material has no normal map (arena 40's ent_shd03 card) is drawn with the neutral one (no bump, half gloss:
  // the effect's own no-bump input) instead of the debug colour; the viewer draws such a material plain.
  const bool neutral_normal = !w.normal && mat.shader.find("BumpMap") != std::string::npos && neutral_normal_ != rdr::kNoImage;
  if (neutral_normal) w.normal = true;
  w.specmap = tex(mat.specular) != rdr::kNoImage;
  w.spec = mat.spec_lev > 0.f;
  w.alpha_test = mat.alpha_test;
  b->choice = ChooseEffect(mat.shader, w);
  b->alpha_test = mat.alpha_test;
  EffectCoverage& cov = coverage_[mat.shader];
  cov.effect = mat.shader;
  if (b->choice.Ok()) {
    if (cov.bound_to.empty()) {
      cov.bound_to = std::string(kVariants[b->choice.vs].name) + " + " + kVariants[b->choice.ps].name;
      for (uint32_t s = 0; s < kVariants[b->choice.ps].sampler_count; ++s)
        if (kVariants[b->choice.ps].samplers[s].slot == 8) cov.bound_to += " [reflection: stand-in, ReflectAlpha 0]";  // S101 lane 15
    }
    b->program = ProgramFor(b->choice.vs, b->choice.ps, mat.alpha_test);
    b->layout = LayoutFor(b->choice.vs);
    const rdr::fx::Variant& fv = kVariants[b->choice.ps];
    for (uint32_t s = 0; s < fv.sampler_count; ++s) {
      const int slotn = fv.samplers[s].slot;
      rdr::ImageId image = white_;
      if (slotn == 0) image = w.diffuse ? tex(mat.diffuse) : white_;
      else if (slotn == 13) image = lm_image, b->slot_clamp[slotn] = 1;
      else if (slotn == 4) image = neutral_normal ? neutral_normal_ : tex(mat.normal);
      else if (slotn == 5) image = tex(mat.specular);
      else if (slotn == 6) image = grey_;
      else if (slotn == 8) image = black_cube_, b->cube_slot[slotn] = 3;  // S101 lane 15: the reflection cube: black = no reflection layer
      if (!options_.textures && slotn != 8) image = grey_;  // the planted fault: no textures
      b->slot_image[slotn] = image == rdr::kNoImage ? white_ : image;
    }
  } else {
    b->debug = true;
    if (std::getenv("WWE_RDR_DUMP_MATS")) std::fprintf(stderr, "[mats] UNBOUND %s tex=%s wants: diffuse=%d lightmap=%d normal=%d specmap=%d spec=%d atest=%d skinned=%d (material slots: normal=%d lm=%d specular=%d has_lm_slot=%d)\n", mat.shader.c_str(), mat.diffuse_name.c_str(), int(w.diffuse), int(w.lightmap), int(w.normal), int(w.specmap), int(w.spec), int(w.alpha_test), int(w.skinned), mat.normal, mat.lightmap, mat.specular, int(mat.has_lm_slot));  // lane 257 (dev)
    if (cov.reason.empty()) cov.reason = b->choice.why_not;
    if (debug_vs_ >= 0) {
      b->program = ProgramFor(debug_vs_, debug_ps_, false);
      b->layout = LayoutFor(debug_vs_);
    }
  }
  m.bound[slot] = std::move(b);
  return *m.bound[slot];
}

// ---- constants by name --------------------------------------------------------------------------------------------------------

namespace {

struct Ctx {
  const float* world;            // 16 floats, row vectors (p' = p * M)
  const float* view_proj;        // 16 floats, row vectors
  const float* eye;
  double time;
  const assets::Material* mat;
  const assets::LightSet* lights;
  const float* lm_xy;            // 2
  float lm_gain;
  float alpha;
  const assets::Matrix* bones = nullptr;  // S101 lane 15: a skinned prop chunk's palette slots (viewer space, world included)
  uint32_t bone_count = 0;
};

// Fills `out` (vec4s * 4 floats, zeroed by the caller). False = the name is not one the front-end knows (reported once).
bool Resolve(const std::string& name, uint32_t vec4s, const Ctx& c, float* out) {
  const auto rows = [&](const float* m, uint32_t n) {  // clip.x = dot(row0, p4): row k = column k of the row-vector matrix
    for (uint32_t k = 0; k < n && k < vec4s; ++k)
      for (int i = 0; i < 4; ++i) out[4 * k + uint32_t(i)] = m[i * 4 + int(k)];
  };
  const assets::Material& mt = *c.mat;
  const assets::LightSet& ls = *c.lights;
  if (name == "ViewProj") { rows(c.view_proj, 4); return true; }
  if (c.bone_count && name == "WorldMatrixArray") {  // S101 lane 15: a skinned prop: slot s = skin[node(s)] * object world (the arena's space trick needs no mirror here)
    for (uint32_t s = 0; s * 3 + 2 < vec4s; ++s) {
      const assets::Matrix& mm = c.bones[std::min(s, c.bone_count - 1)];
      for (uint32_t k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i) out[4 * (3 * s + k) + uint32_t(i)] = mm[size_t(i * 4) + k];
    }
    return true;
  }
  if (c.bone_count && name == "Bools") {  // fx_prop.glsl prop_skin tests b4, then b0..b3: bit 3 alone = four bones (weights.xyz + the rest)
    const uint32_t bits = 0x8u;
    std::memcpy(out, &bits, 4);
    return true;
  }
  if (name == "WorldMatrixArray" || name == "WorldMatrix") {
    rows(c.world, 3);
    for (uint32_t m = 1; m * 3 + 2 < vec4s; ++m) std::memcpy(out + 12 * m, out, 12 * sizeof(float));  // a bone palette of a rigid draw: every slot the world
    return true;
  }
  if (name == "MatAmbCol") { std::memcpy(out, mt.ambient, 16); return true; }
  if (name == "MatDifCol") { std::memcpy(out, mt.diffuse_color, 16); return true; }
  if (name == "MdlAlpha") { out[0] = c.alpha; return true; }
  if (name == "DifLghCol") {
    for (uint32_t k = 0; k < 4 && k < vec4s; ++k) for (int i = 0; i < 3; ++i) out[4 * k + uint32_t(i)] = ls.col[k][i];
    return true;
  }
  // SPACE (SCENE_FRONTEND.md "Space"): the effects are written for the game's file space (-Y up, the stored normals point INTO the surface, the
  // light directions are the way the light travels). The viewer's vertices and normals are the file's with Y negated, so the light directions
  // are negated in Y to match, and the sky axis is -Y: every dot product the effect takes is then the file-space one.
  if (name == "DifLghVec") {
    for (uint32_t k = 0; k < 4 && k < vec4s; ++k) {
      out[4 * k] = ls.dir[k][0], out[4 * k + 1] = -ls.dir[k][1], out[4 * k + 2] = ls.dir[k][2];
    }
    return true;
  }
  if (name == "vMin" || name == "fvMin" || name == "LightMin") return true;  // 0: no floor on n.l
  if (name == "SkyDir") { out[1] = -1.f; return true; }
  if (name == "SkyCol") { for (int i = 0; i < 3; ++i) out[i] = ls.sky[i]; out[3] = 1.f; return true; }
  if (name == "GroundCol") { for (int i = 0; i < 3; ++i) out[i] = ls.ground[i]; out[3] = 1.f; return true; }
  if (name == "LightMapParam") { out[0] = c.lm_xy[0]; out[1] = c.lm_xy[1]; out[2] = c.lm_gain; out[3] = 1.f; return true; }
  if (name == "EyePos") { out[0] = c.eye[0]; out[1] = c.eye[1]; out[2] = c.eye[2]; out[3] = 1.f; return true; }
  if (name == "Time") { out[0] = float(c.time); return true; }
  if (name == "UVScroll0" || name == "UVScroll1") return true;  // the file's scroll speeds are not read yet: a still texture
  if (name == "MipmapBias") return true;
  if (name == "SpecularCol") {
    std::memcpy(out, mt.spec_col, 16);
    // S101 lane 257: the highlight is the FIRST light's: its colour scales it (viewer_hlsl.h: "rgb += a * spec.w * spec.rgb * pow(...) * lCol[0].rgb"). The effects' pixel stage has no light
    // colour term, so it goes into SpecularCol here; a mesh whose light 0 is off (the NXT walls, curtains: colour 0) has no highlight. Without it every material with the file's
    // default-looking lobe (level 0.5, power 5) washed floors and curtains white from the back views (census tile faults, lane 257). WWE_RDR_SPEC_NO_LIGHTCOL=1 = the old fault (plant).
    static const bool plant = std::getenv("WWE_RDR_SPEC_NO_LIGHTCOL") != nullptr;
    if (!plant) for (int i = 0; i < 3; ++i) out[i] *= ls.col[0][i];
    return true;
  }
  if (name == "SpecularLev") { static const bool no_spec = std::getenv("WWE_RDR_NO_SPEC") != nullptr; out[0] = no_spec ? 0.f : mt.spec_lev; return true; }
  if (name == "SpecularPow" || name == "iSpecularPow") { out[0] = mt.spec_pow; return true; }
  if (name == "ReflectAlpha" || name == "Noise" || name == "NoiseParam" || name == "ShadowPrm" || name == "CameraViewProj" || name == "PlaneInv" ||
      name == "TexProjLight" || name == "ViewLight" || name == "ConvScreen" || name == "SpeLodParam" || name == "ChangeColor")
    return true;  // zero: the option that reads them is not chosen, or the value is the neutral one
  return false;
}

}  // namespace

// S101 lane 257 (dev diagnostics, off unless the variable is set): WWE_RDR_SKIP_EFFECT=a,b leaves those effect classes out of the frame; WWE_RDR_ONLY_EFFECT=a,b draws only them.
// WWE_RDR_NO_SPEC=1 zeroes every material's specular level (A/B for the highlight terms). Used by tools\filecensus\diag.py to find which draws a tile fault comes from.
static bool DebugEffectFilter(const std::string& effect) {
  static const std::string skip = [] { const char* e = std::getenv("WWE_RDR_SKIP_EFFECT"); return std::string(e ? e : ""); }();
  static const std::string only = [] { const char* e = std::getenv("WWE_RDR_ONLY_EFFECT"); return std::string(e ? e : ""); }();
  const auto listed = [&](const std::string& list) {
    size_t p = 0;
    while (p <= list.size()) {
      size_t q = list.find(',', p);
      if (q == std::string::npos) q = list.size();
      if (q > p && list.compare(p, q - p, effect) == 0) return true;
      p = q + 1;
    }
    return false;
  };
  if (!skip.empty() && listed(skip)) return true;
  if (!only.empty() && !listed(only)) return true;
  return false;
}

// ---- the frame ----------------------------------------------------------------------------------------------------------------

// S101 lane 13: the ambient cube of a light set (the wrestlers' diffuse light), built once per distinct set.
const FrontEnd::CubeEntry& FrontEnd::CubeFor(const assets::LightSet& ls) {
  const uint64_t key = LightSetKey(ls);
  const auto found = cubes_.find(key);
  if (found != cubes_.end()) return found->second;
  if (cubes_.size() >= 32) {  // a lighting editor drags a set through many values: start over rather than grow
    renderer_.WaitIdle();
    for (auto& kv : cubes_)
      if (kv.second.image != rdr::kNoImage) renderer_.DestroyImage(kv.second.image);
    cubes_.clear();
  }
  CubeEntry e;
  std::vector<uint16_t> half;
  BuildAmbientCube(ls, kCubeSize, half, e.scale);
  rdr::ImageDesc d;
  d.width = d.height = kCubeSize;
  d.layers = 6;
  d.kind = rdr::ImageKind::Cube;
  d.format = rdr::Format::RGBA16F;
  d.usage = rdr::kUsageSampled;
  e.image = renderer_.CreateImage(d);
  const size_t face_bytes = size_t(kCubeSize) * kCubeSize * 8;
  for (uint32_t face = 0; face < 6 && e.image != rdr::kNoImage; ++face)
    renderer_.UpdateImage(e.image, 0, face, reinterpret_cast<const uint8_t*>(half.data()) + face * face_bytes, face_bytes);
  return cubes_.emplace(key, e).first->second;
}

void FrontEnd::EnsureTargets(uint32_t w, uint32_t h) {
  if (color_ != rdr::kNoImage && target_w_ == w && target_h_ == h && depth_sampled_ == want_depth_sampled_) return;  // S101 lane 34: a sampled (non-transient) depth when the post pass reads it
  renderer_.WaitIdle();
  depth_sampled_ = want_depth_sampled_;
  if (color_ != rdr::kNoImage) renderer_.DestroyImage(color_);
  if (depth_ != rdr::kNoImage) renderer_.DestroyImage(depth_);
  if (post_out_ != rdr::kNoImage) renderer_.DestroyImage(post_out_), post_out_ = rdr::kNoImage;
  if (live_screen_ != rdr::kNoImage) renderer_.DestroyImage(live_screen_), live_screen_ = rdr::kNoImage;  // S101 lane 257
  live_valid_ = false;
  rdr::ImageDesc c;
  c.width = uint16_t(w);
  c.height = uint16_t(h);
  c.format = rdr::Format::RGBA8;
  c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  color_ = renderer_.CreateImage(c);
  live_screen_ = renderer_.CreateImage(c);  // S101 lane 257: the big screen's copy of the last frame
  rdr::ImageDesc d = c;
  d.format = rdr::Format::Depth;
  d.usage = depth_sampled_ ? uint8_t(rdr::kUsageTarget | rdr::kUsageSampled) : uint8_t(rdr::kUsageTarget | rdr::kUsageTransient);
  depth_ = renderer_.CreateImage(d);
  if (depth_sampled_) post_out_ = renderer_.CreateImage(c);  // S101 lane 34: the post pass's output (the scene colour's description)
  target_w_ = w, target_h_ = h;
}

// S101 lane24: the programs are made again at the next draw (the GPU is idle first: the old pipelines may still be in flight).
void FrontEnd::ReloadPrograms(const std::vector<int>& changed) {
  renderer_.WaitIdle();
  // Only the programs that use a changed variant are made again (the others keep their ids: the renderer's program table only grows, 16 bit).
  const size_t before = programs_.size();
  for (auto it = programs_.begin(); it != programs_.end();) {
    const int vs = int(it->first >> 32), ps = int((it->first >> 1) & 0x7FFFFFFFu) - 1;
    if (std::find(changed.begin(), changed.end(), vs) != changed.end() || std::find(changed.begin(), changed.end(), ps) != changed.end()) it = programs_.erase(it);
    else ++it;
  }
  if (post_pass_) post_pass_->Reload(changed);  // S101 lane 34 review F: the post pass caches program ids the same way
  std::fprintf(stderr, "[reload] %zu of %zu programs forgotten (they use one of the %zu changed variants); the rest keep their ids\n", before - programs_.size(), before, changed.size());
  for (ModelData* m : models_)
    if (m)
      for (auto& b : m->bound) b.reset();
}

bool FrontEnd::RenderToPixels(const SceneFrame& frame, std::vector<uint8_t>& rgba, std::string& error) {
  if (ShaderReload* reload = ShaderReload::Get())  // S101 lane24: adopts a finished reload before the frame is built
    if (std::vector<int> changed; reload->Poll(&changed)) ReloadPrograms(changed);
  if (frame.width == 0 || frame.height == 0 || frame.width > 16384 || frame.height > 16384) { error = "rdr: bad frame size"; return false; }
  // S101 lane 257: a match-look frame with the stage's big screen in it first draws one seed frame (no post), so the screen shows the picture of this scene, not black (the viewer's rule)
  bool have_screen = false;
  static const bool no_live_screen = std::getenv("WWE_RDR_NO_LIVE_SCREEN") != nullptr;  // lane 257 plant: the screen stays black (the census must fail)
  if (!frame.entrance && !no_live_screen)
    for (const Object& obj : frame.objects) {
      if (!(obj.flags & kObjVisible) || obj.asset >= models_.size() || !models_[obj.asset]->live) continue;
      for (const ModelData::Mesh& me : models_[obj.asset]->meshes) have_screen = have_screen || me.part == "slot 800";
    }
  if (have_screen && !in_seed_) {
    in_seed_ = true;
    SceneFrame seed = frame;
    seed.post = nullptr;
    std::vector<uint8_t> ignored;
    const bool ok = RenderToPixels(seed, ignored, error);
    in_seed_ = false;
    if (!ok) return false;
  }
  want_depth_sampled_ = frame.post != nullptr;  // S101 lane 34
  EnsureTargets(frame.width, frame.height);
  if (color_ == rdr::kNoImage || depth_ == rdr::kNoImage) { error = "rdr: could not create the frame's images"; return false; }

  // The camera: view * proj (the viewer hands its combined matrix as `view` and the identity as `proj`).
  float vp[16];
  {
    const auto& a = frame.camera.view.m;
    const auto& b = frame.camera.proj.m;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        vp[i * 4 + j] = s;
      }
  }
  const float* eye = frame.camera.eye;

  struct Item {
    ModelData* model;
    const ModelData::Mesh* mesh;
    assets::Matrix world;
    uint8_t blend;      // 0 off, 1 alpha, 2 additive
    bool depth_write;
    bool sorted;        // drawn in the far-to-near list
    float dist2;
    float alpha;
    int light_override;
    const ModelLights* ml = nullptr;  // S101 lane 15: this frame's light sets of the model + each mesh's choice (null = the ones it was added with)
    // S101 lane 13 (characters)
    uint32_t object = 0;           // index into `objs`
    uint8_t pass = 0;              // hair / translucent: 1 = back faces culled, 2 = front faces culled (the viewer draws such a mesh twice)
  };
  struct ObjInfo {                 // per drawn object: its palette (skeleton node -> viewer-space matrix, world included) and light set
    assets::Matrix world;
    const Mat4* skin = nullptr;
    uint32_t skin_count = 0;
    const assets::LightSet* light = nullptr;
  };
  std::vector<ObjInfo> objs;
  objs.reserve(frame.objects.size());
  std::vector<Item> opaque, blended;
  // S101 lane 15 (entrances): the light sets of a model as the entrance rig rewrote them this frame (the last entry = the default rig)
  std::vector<const ModelLights*> model_sets(models_.size(), nullptr);
  for (const ModelLights& ml : frame.model_lights)
    if (ml.asset < models_.size()) model_sets[ml.asset] = &ml;
  for (const Object& obj : frame.objects) {
    if (!(obj.flags & kObjVisible)) continue;
    if (obj.asset >= models_.size() || !models_[obj.asset]->live) continue;
    ModelData& m = *models_[obj.asset];
    assets::Matrix ow;
    std::memcpy(ow.data(), obj.world.m, sizeof(float) * 16);
    ObjInfo oi;
    oi.world = ow;
    if ((obj.flags & kObjSkinned) && obj.bones_first + size_t(obj.bones_count) <= frame.bones.size() && obj.bones_count)
      oi.skin = &frame.bones[obj.bones_first], oi.skin_count = obj.bones_count;
    if (obj.light_set < frame.light_sets.size()) oi.light = &frame.light_sets[obj.light_set];
    objs.push_back(oi);
    const uint32_t object_index = uint32_t(objs.size() - 1);
    for (const ModelData::Mesh& me : m.meshes) {
      // S101 lane 13: which of the model's first 64 meshes are shown (attire pieces, the eyelid layer)
      if (me.src < 32 ? !((obj.mesh_mask_lo >> me.src) & 1u) : (me.src < 64 && !((obj.mesh_mask_hi >> (me.src - 32)) & 1u))) continue;
      if (me.skinned && !me.chunk) {  // a skinned piece of a class that is not a character: its bone inputs are not wired (counted, not drawn)
        EffectCoverage& cov = coverage_[me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)"];
        cov.effect = me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)";
        if (cov.reason.empty()) cov.reason = "skinned piece of a class that is not a yCh_* effect: skinning is wired for the characters' effects only";
        ++cov.meshes_unbound;
        continue;
      }
      const assets::Material* mt = me.material >= 0 ? &m.materials[size_t(me.material)] : nullptr;
      const bool sorted = mt && mt->alpha_blend;
      const size_t instances = me.placement >= 0 ? m.placements[size_t(me.placement)].size() : 1;
      for (size_t k = 0; k < instances; ++k) {
        Item it;
        it.model = &m;
        it.mesh = &me;
        it.world = me.placement >= 0 ? assets::Mul(m.placements[size_t(me.placement)][k], ow) : ow;
        it.blend = !mt ? 0 : sorted ? (mt->additive ? 2 : 1) : mt->blend_inline ? 1 : 0;
        it.depth_write = !sorted;
        it.sorted = sorted;
        it.alpha = obj.tint[3];
        it.light_override = -1;
        it.ml = model_sets[obj.asset];
        it.object = object_index;
        it.pass = 0;
        const float cx = me.center[0] * it.world[0] + me.center[1] * it.world[4] + me.center[2] * it.world[8] + it.world[12] - eye[0];
        const float cy = me.center[0] * it.world[1] + me.center[1] * it.world[5] + me.center[2] * it.world[9] + it.world[13] - eye[1];
        const float cz = me.center[0] * it.world[2] + me.center[1] * it.world[6] + me.center[2] * it.world[10] + it.world[14] - eye[2];
        it.dist2 = cx * cx + cy * cy + cz * cz;
        if (sorted && mt && IsCharEffect(mt->shader) && IsHairEffect(mt->shader)) {  // the game draws such a mesh twice (renderer.cpp BuildSceneList)
          it.pass = 1;
          blended.push_back(it);
          it.pass = 2;
          blended.push_back(it);
        } else {
          (sorted ? blended : opaque).push_back(it);
        }
      }
    }
  }
  std::stable_sort(blended.begin(), blended.end(), [](const Item& a, const Item& b) { return a.dist2 > b.dist2; });

  builder_.Begin(0);
  rdr::TargetsOp targets;
  targets.color[0].image = color_;
  targets.color_count = 1;
  targets.depth.image = depth_;
  builder_.SetTargets(targets);
  rdr::ClearOp clear;
  clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;
  clear.full = 1;
  for (int i = 0; i < 4; ++i) clear.color[i] = frame.clear[i];
  clear.depth = 1.0f;
  builder_.Clear(clear);

  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(frame.width);
  dynamic.viewport[3] = float(frame.height);
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = frame.width;
  dynamic.scissor.height = frame.height;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;  // clip y is up (D3D-style matrices); the viewport is Vulkan's
  dynamic.system[9] = 7.f;
  rdr::DynamicState dynamic_alpha = dynamic;
  dynamic_alpha.system[8] = 0.5f;   // alpha test: reference 0.5
  dynamic_alpha.system[9] = 6.f;    // greater or equal (the viewer's clip(a - 0.5))

  const assets::LightSet default_rig = assets::DefaultRig();
  const float debug_colour[4] = {1.f, 0.f, 1.f, 1.f};
  std::vector<uint8_t> block;
  std::set<uint64_t> cloth_done;  // the cloth pieces posed and uploaded this frame (a hair / translucent piece is drawn twice from one upload)
  const auto emit = [&](const Item& it) {
    ModelData& m = *it.model;
    const ModelData::Mesh& me = *it.mesh;
    if (DebugEffectFilter(me.material >= 0 ? m.materials[size_t(me.material)].shader : std::string("(no material)"))) return;
    if (static const std::string skip_tex = [] { const char* e = std::getenv("WWE_RDR_SKIP_TEX"); return std::string(e ? e : ""); }(); !skip_tex.empty() && me.material >= 0 &&
        skip_tex.find(m.materials[size_t(me.material)].diffuse_name) != std::string::npos && !m.materials[size_t(me.material)].diffuse_name.empty()) return;  // S101 lane 257 (dev): WWE_RDR_SKIP_TEX=name1,name2 leaves the meshes with those diffuse textures out  // S101 lane 257: WWE_RDR_SKIP_EFFECT / WWE_RDR_ONLY_EFFECT (dev)
    const bool ent_lm = frame.entrance && me.material >= 0 && EntranceLmClass(m.materials[size_t(me.material)], frame.entrance_bg) && !IsMovieSurface(me.part);  // S101 lane 15
    const Bound& b = BindMaterial(m, me.material, me.skinned && me.chunk, ent_lm);
    if (b.debug && (!options_.debug_unbound || debug_vs_ < 0)) { ++coverage_[me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)"].meshes_unbound; return; }
    EffectCoverage& cov = coverage_[me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)"];
    (b.debug ? cov.meshes_unbound : cov.meshes_bound) += 1;
    assets::Material fallback;
    const assets::Material& mat = me.material >= 0 ? m.materials[size_t(me.material)] : fallback;
    // S101 lane 15: this frame's sets when the caller gave them (the entrance rig), else the ones the model was added with
    // The viewer's belts and carried props: the entrant's set is appended and the normal-mapped meshes re-pointed at it (SetMeshLightsByNormal), so the mesh's own
    // index (ModelLights::mesh_set) decides; a mesh with no entry keeps the file's choice, and -1 is the default rig (never the last set: that may be the entrant's).
    const std::vector<assets::LightSet>& sets = it.ml ? it.ml->sets : m.light_sets;
    int set_index = me.light_set;
    if (it.ml && size_t(me.src) < it.ml->mesh_set.size()) set_index = it.ml->mesh_set[size_t(me.src)];
    const assets::LightSet& ls = (set_index >= 0 && size_t(set_index) < sets.size()) ? sets[size_t(set_index)] : default_rig;
    Ctx ctx{it.world.data(), vp, eye, frame.time_s, &mat, &ls, m.lm_xy, m.lm_gain, it.alpha};
    if (static const bool dump = std::getenv("WWE_RDR_DUMP_MATS") != nullptr; dump && !b.debug && me.material >= 0) {  // S101 lane 257 (dev): one line per distinct material: what the draw is fed
      static std::set<const void*> seen;
      if (seen.insert(&me).second) {
        const float* w = it.world.data();  // the sign of the 3x3 determinant: negative = a mirrored placement (the winding flips)
        const float det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) + w[2] * (w[4] * w[9] - w[5] * w[8]);
        std::fprintf(stderr, "[mats] %-14s part=%-10s tex=%-14s vs=%s ps=%s spec_lev=%.2f pow=%.0f spec_col=%.1f light0=%.2f lm=%d atest=%d cull=%d blend=%d sorted=%d inl=%d ambw=%.2f difw=%.2f det=%s\n", mat.shader.c_str(), me.part.c_str(), mat.diffuse_name.c_str(),
                     kVariants[b.choice.vs].name, kVariants[b.choice.ps].name, mat.spec_lev, mat.spec_pow, mat.spec_col[0], ls.col[0][0], int(mat.lightmap >= 0), int(mat.alpha_test), int(mat.cull_back), int(it.blend), int(it.sorted), int(mat.blend_inline), mat.ambient[3], mat.diffuse_color[3], det < 0 ? "NEG" : "pos");
      }
    }
    // S101 lane 13 (characters): the bone matrices of this piece (palette slot -> world-included viewer-space matrix), its light set and ambient cube.
    const ObjInfo& oi = objs[it.object];
    std::vector<assets::Matrix> slots;
    const assets::LightSet* char_light = nullptr;
    const CubeEntry* cube = nullptr;
    const bool skin_prop = !b.chr && me.chunk;  // S101 lane 15: a skinned prop chunk (bound: the effect skins it; debug colour: posed on the CPU below)
    if ((b.chr && !b.debug) || skin_prop) {
      if (me.chunk) {
        slots.reserve(me.nodes.size());
        for (uint16_t node : me.nodes) {
          assets::Matrix sk = assets::Identity();
          if (oi.skin && node < oi.skin_count) std::memcpy(sk.data(), oi.skin[node].m, sizeof(float) * 16);
          slots.push_back(assets::Mul(sk, oi.world));  // (skin, then the object's place: row vectors)
        }
      }
      if (slots.empty()) slots.push_back(it.world);  // a rigid piece of a character model: its world in every slot
      if (b.chr) {
        char_light = oi.light ? oi.light : &default_rig;
        cube = &CubeFor(*char_light);
      }
    }
    if (skin_prop && !b.debug) ctx.bones = slots.data(), ctx.bone_count = uint32_t(slots.size());
    const bool debug_posed = b.debug && skin_prop && !me.cpu.empty() && !slots.empty();  // the debug colour draws the piece where the palette puts it
    CharCtx cc;
    cc.mat = &mat, cc.lights = char_light, cc.view_proj = vp, cc.eye = eye, cc.bones = slots.data(), cc.bone_count = uint32_t(slots.size()), cc.alpha = it.alpha;
    cc.cube_scale = cube ? cube->scale : nullptr;
    cc.cloth_offset = b.chr && !me.chunk && IsClothEffect(mat.shader);  // a rigid cloth piece: the bone matrix is an offset
    cc.mirror = frame.viewer_space ? -1.f : 1.f;

    const int vs = b.debug ? debug_vs_ : b.choice.vs, ps = b.debug ? debug_ps_ : b.choice.ps;
    const rdr::fx::Variant& vv = kVariants[vs];
    const rdr::fx::Variant& fv = kVariants[ps];
    const auto pack = [&](int variant, const rdr::fx::Variant& v) {
      const fxt::VariantTable* t = T().by_index[size_t(variant)];
      const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(v.block_bytes, 16), 8192);
      block.assign(bytes, 0);
      if (t) {
        for (uint32_t k = 0; k < t->field_count; ++k) {
          const fxt::Field& f = t->fields[k];
          if (f.offset + f.vec4s * 16u > bytes) continue;
          float* out = reinterpret_cast<float*>(block.data() + f.offset);
          const std::string fname = f.name;
          if (b.debug) {
            if (fname == "mWVP") {  // world * view-projection, as rows
              assets::Matrix wvp = assets::Mul(debug_posed ? assets::Identity() : it.world, [&] { assets::Matrix p; std::memcpy(p.data(), vp, sizeof vp); return p; }());
              for (int r = 0; r < 4; ++r) for (int i = 0; i < 4; ++i) out[4 * r + i] = wvp[size_t(i * 4 + r)];
            } else if (fname == "Color") {
              std::memcpy(out, debug_colour, 16);
            }
            continue;
          }
          if (b.chr ? !ResolveChar(fname, f.vec4s, cc, out) : !Resolve(fname, f.vec4s, ctx, out)) missing_fields_.insert(fname);
        }
      }
      return builder_.PushConstants(block.data(), bytes);
    };

    rdr::DrawState state{};
    state.program = b.program;
    state.vertex_layout = b.layout;
    state.topology = 3;  // triangle list
    const assets::Material* mp = me.material >= 0 ? &mat : nullptr;
    state.cull = (mp && mp->cull_back) ? 2 : 0;  // back faces culled (the file's rule); two-sided otherwise
    state.front_face = 1;  // clockwise on screen (the viewer's rule)
    if (it.pass) state.cull = it.pass == 1 ? 2 : 1;  // S101 lane 13: hair / translucent, drawn twice: back faces culled, then front faces
    state.depth_test = 1;
    state.depth_write = it.depth_write ? 1 : 0;
    state.depth_compare = 3;  // less or equal
    state.blend[0].write_mask = 0xF;
    if (it.blend) {
      state.blend[0].enable = 1;
      state.blend[0].src_color = 6;  // src alpha
      state.blend[0].dst_color = it.blend == 2 ? 1 : 7;  // one / one minus src alpha
      state.blend[0].src_alpha = 1;
      state.blend[0].dst_alpha = 7;
    }
    rdr::TextureSet set{};
    for (uint32_t s = 0; !b.debug && s < fv.sampler_count; ++s) {
      const int slot = fv.samplers[s].slot;
      rdr::SamplerDesc sd{};
      sd.mag_filter = sd.min_filter = 1;
      sd.mip_mode = 1;
      sd.address_u = sd.address_v = sd.address_w = (b.slot_clamp[slot] || b.cube_slot[slot]) ? 2 : 0;
      sd.max_anisotropy = 8;
      sd.max_lod = 1000.f;
      set.slots[slot].image = b.cube_slot[slot] == 1 ? (cube ? cube->image : b.slot_image[slot]) : b.slot_image[slot];  // the ambient cube is per object
      set.slots[slot].sampler = builder_.Sampler(sd);
      set.slots[slot].kind = fv.samplers[s].kind;
    }
    if (live_valid_ && !frame.entrance && me.part == "slot 800" && live_screen_ != rdr::kNoImage && fv.sampler_count && set.slots[0].image != rdr::kNoImage) set.slots[0].image = live_screen_;  // S101 lane 257
    rdr::DrawOp op{};
    op.state = builder_.State(state);
    op.dynamic = builder_.Dynamic(b.alpha_test ? dynamic_alpha : dynamic);
    op.textures = builder_.Textures(set);
    op.vs_constants = pack(vs, vv);
    op.fs_constants = pack(ps, fv);
    op.vertex[0] = {m.vb, me.vb_offset};
    if ((me.cloth && !b.debug && !slots.empty()) || debug_posed) {
      // S101 lane 13: skinned cloth is posed here (the viewer skins it like any piece; the effect takes its position as it is, in the game's space)
      const uint64_t key = (uint64_t(it.object) << 32) | uint32_t(&me - m.meshes.data());
      rdr::BufferId& buf = m.cloth_bufs[key];
      if (cloth_done.insert(key).second || buf == rdr::kNoBuffer) {
      std::vector<assets::Vertex> posed = me.cpu;
      const float mir = (frame.viewer_space && !debug_posed) ? -1.f : 1.f;  // (the debug colour stays in the viewer's space)
      for (assets::Vertex& v : posed) {
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, wsum = 0;
        for (int k = 0; k < 4; ++k) {
          const float w = v.weights[k];
          if (!(w > 0.f)) continue;
          size_t sl = size_t(std::find(me.nodes.begin(), me.nodes.end(), v.bones[k]) - me.nodes.begin());
          if (sl >= slots.size()) sl = 0;
          const assets::Matrix& M = slots[sl];
          for (int c = 0; c < 3; ++c) {
            p[c] += w * (v.pos[0] * M[size_t(c)] + v.pos[1] * M[size_t(4 + c)] + v.pos[2] * M[size_t(8 + c)] + M[size_t(12 + c)]);
            n[c] += w * (v.normal[0] * M[size_t(c)] + v.normal[1] * M[size_t(4 + c)] + v.normal[2] * M[size_t(8 + c)]);
          }
          wsum += w;
        }
        if (!(wsum > 0.f)) continue;
        v.pos[0] = p[0], v.pos[1] = mir * p[1], v.pos[2] = p[2];
        v.normal[0] = n[0], v.normal[1] = mir * n[1], v.normal[2] = n[2];
      }
      const size_t bytes = posed.size() * sizeof(assets::Vertex);
      if (buf == rdr::kNoBuffer) buf = renderer_.CreateBuffer(bytes, posed.data());
      else renderer_.UpdateBuffer(buf, 0, posed.data(), bytes);
      }
      op.vertex[0] = {buf, 0};
    }
    if (me.chunk) op.vertex[1] = {m.svb, me.sv_offset};  // S101 lane 13: palette slots + weights
    op.index = {m.ib, me.ib_offset};
    op.index_32bit = 1;
    op.count = me.index_count;
    op.instances = 1;
    builder_.Draw(op);
  };
  for (const Item& it : opaque) emit(it);
  for (const Item& it : blended) emit(it);

  if (have_screen && live_screen_ != rdr::kNoImage) {  // S101 lane 257: this frame's scene colour becomes the next frame's screen picture
    rdr::CopyOp cp{};
    cp.source.image = color_, cp.dest.image = live_screen_;
    cp.source_rect = {0, 0, frame.width, frame.height};
    cp.dest_rect = cp.source_rect;
    builder_.Copy(cp);
    live_valid_ = true;
  }

  // S101 lane 34: the post pass (depth of field, haze, bloom, glare) on the scene's colour and depth
  if (frame.post) {
    if (!post_pass_) post_pass_ = std::make_unique<PostPass>(renderer_);
    PostParams pp = *frame.post;
    pp.width = frame.width, pp.height = frame.height;
    if (!post_pass_->Record(builder_, color_, depth_, post_out_, pp, error)) return false;
  }
  renderer_.RenderFrame(builder_.Finish());
  renderer_.WaitIdle();
  uint32_t w = 0, h = 0;
  rdr::ImageRef ref;
  ref.image = frame.post ? post_out_ : color_;
  if (!renderer_.ReadImage(ref, rgba, w, h) || w != frame.width || h != frame.height) { error = "rdr: could not read the frame back"; return false; }
  return true;
}

// S101 lane 15: a live texture (the entrance's tron video frames, the stage light-map page) takes a new picture. The caller gives RGBA8 of the
// texture's own size; the GPU is idle between frames (RenderToPixels waits).
bool FrontEnd::UpdateTexture(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba) {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  if (texture >= m.images.size() || m.images[texture] == rdr::kNoImage || !rgba || !w || !h) return false;
  const ModelData::ImgInfo& ii = m.img_info[texture];
  if (!ii.rgba8 || ii.mips != 1 || ii.w != w || ii.h != h) return false;  // only a 1-mip RGBA8 image of exactly this size (a live texture) is replaced
  renderer_.UpdateImage(m.images[texture], 0, 0, rgba, size_t(w) * h * 4);
  ++texture_updates_;
  return true;
}

// ---- coverage -----------------------------------------------------------------------------------------------------------------

std::vector<EffectCoverage> FrontEnd::Coverage() const {
  std::vector<EffectCoverage> out;
  for (const auto& kv : coverage_)
    if (kv.second.meshes_bound + kv.second.meshes_unbound > 0 || !kv.second.bound_to.empty() || !kv.second.reason.empty()) out.push_back(kv.second);
  return out;
}

std::string FrontEnd::CoverageText() const {
  std::string s;
  char line[512];
  uint32_t bound = 0, unbound = 0, effects_bound = 0, effects_total = 0;
  for (const EffectCoverage& c : Coverage()) {
    ++effects_total;
    const bool ok = !c.bound_to.empty();
    effects_bound += ok;
    bound += c.meshes_bound, unbound += c.meshes_unbound;
    if (ok) std::snprintf(line, sizeof line, "  %-18s bound to our effect   %s  (%u draws)\n", c.effect.c_str(), c.bound_to.c_str(), c.meshes_bound);
    else std::snprintf(line, sizeof line, "  %-18s no effect yet         %s  (%u draws in the debug colour)\n", c.effect.c_str(), c.reason.c_str(), c.meshes_unbound);
    s += line;
    if (ok && c.meshes_unbound) {  // some materials of the class could not be bound (another feature set): said, not hidden
      std::snprintf(line, sizeof line, "    also %u draws of %s in the debug colour: %s\n", c.meshes_unbound, c.effect.c_str(), c.reason.c_str());
      s += line;
    }
  }
  std::snprintf(line, sizeof line, "effects: %u of %u bound to our effect; draws: %u bound, %u debug colour\n", effects_bound, effects_total, bound, unbound);
  s += line;
  if (!missing_fields_.empty()) {
    s += "  constants not filled (zero):";
    for (const std::string& f : missing_fields_) s += " " + f;
    s += "\n";
  }
  return s;
}

void FrontEnd::ResetCoverage() {
  coverage_.clear();
  missing_fields_.clear();
  for (ModelData* m : models_)
    for (auto& b : m->bound) b.reset();  // re-bind (the coverage is filled while binding)
}

}  // namespace rdr_scene
