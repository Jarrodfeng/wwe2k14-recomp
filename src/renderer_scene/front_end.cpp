// The scene front-end: scene record tables -> renderer frames. See front_end.h and docs\renderer\SCENE_FRONTEND.md.
#include "front_end.h"
#include "../renderer_host/wide_frame.h"   // S105 A2: Box / BoxOf (pure)
#include "bone_tag_rows.h"   // S103 W2B: g_spring_census
#include "splat_state.h"   // S108 BLD2: DepthWriteOf
#include "card_gain.h"  // S103 B3c
#include "liquid_join.h"   // S103 SWF
#include "liquid_switch.h"   // S103 SWF: Plants (the rig's dev plants)
#include "liquid_gate.h"     // S111 GEARLIQUID: which character draws take the body's liquid map at slot 14
#include "props_map_switch.h"   // S103 PMS: the props map switch
#include "live_particles.h"   // S103 PRT: HasSoft
#include "model_group.h"  // S103 GSTP: GroupOfLabel (the fallback group of a model nobody named)
#include "card_atlas_rule.h"  // S103 CB1
#include "family_cull.h"  // S105 C3: the one cull rule for every model family
#include "../keep_stream.h"  // S103 GT: WWE2K14_STREAM()

#include <algorithm>
#include <atomic>  // S103 E6
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"
#include "../renderer_adapter/rdr_switches.h"   // S103 UP: wwe2k14_rdr_page_per_caller (no game or SDK includes)
#include "own_blend.h"   // S103 P7: the blend tag of an own draw (the host's tween)
#include "record_io.h"   // S103 P7 (review F1): SceneTables::Str for the own blend key
#include "rows_inputs.h"   // S106 G4-2
#include "feature_dump.h"   // S103 T2: WWE_RDR_FEATURE_DUMP (dev)
#include "pass_state.h"     // S103 B2: the game's per-pass render state (alpha ref / func, depth func) as a named table
#include "read_rule.h"      // S103 D1: the read rule
#include "sampler_table.h"  // S103 B1: the game's sampler state per (effect, stage)
#include "technique_pick.h"  // S103 TQF: the pixel variant of a yProp draw from the technique word
#include "part_clock_lookup.h"   // S103 E5
#include "piece_row_rule.h"   // S105 A5b: MeshAnswersPieceRow
#include "piece_draw_rule.h"  // S108 HAIR: a wrestler piece's translucent mesh = GEQUAL 128 + depth write (sub_8259E488)
#include "shader_reload.h"  // S101 lane24
#include "upload_check.h"   // S108 AL1: the pure upload refusals + their draw_shortfall report
#include "game_levels.h"   // S111 MIPLEVELS: the game's level count of a file texture (Relevel)
#include "change_colour_draw.h"   // S108 THR2: the Threads editor's live colours (the bind's masks, ChangeColor)
#include "muscle_map.h"   // S111 MUSCLEMAP: slot 2 of an FX_MUSCLE fragment = the material's muscle map
#include "env_cube.h"   // S111 ENVCUBE: slot 10 of a character draw = the material's g_mEnvLPrm cube
#include "body_order.h"   // S111 FADEORDER: the opaque list's turns around the wrestler bodies (OrderOpaque)
#include "piece_owner.h"   // S111 FADEORDER review: OwnerBodySlot / kObjPieceLinkFlags (a wrestler object's character slot: the Body turn's order)
#include "char_alpha.h"   // S111 CHALPHA: a wrestler colour draw is tested + blended only where its alpha can fall below 1
#include "../renderer/shaders/spv/rdr_flash_vert.h"   // S110 CUTGREY (front_end_flashes.h)
#include "../renderer/shaders/spv/rdr_flash_frag.h"
#include "../scene_live/prop_colour.h"   // S108 THB: the entrance props' colours (ChangeColor of a prop draw, the census)

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;

// S103 B2: the dynamic state of an alpha-tested draw of pass class `c` (pass_state.h). `plant_old` = the planted twin of the old behaviour: one 0.5 reference for every class (the rig sets it).
rdr::DynamicState AlphaDynamic(const rdr::DynamicState& base, pass_state::PassClass c, bool plant_old) {
  rdr::DynamicState d = base;
  d.system[8] = plant_old ? 0.5f : pass_state::Of(c).alpha_ref;
  d.system[9] = plant_old ? 6.f : float(pass_state::Of(c).alpha_test_func);   // the Xenos function bits = VkCompareOp numbers (6 = greater or equal)
  return d;
}
// S103 B2: the game's arena class tests + blends EVERY draw (GEQUAL 1/255, census). That changes the picture only when the part's output alpha carries a texture alpha that is not 1:
// Variant::out_alpha == 2 (the fragment multiplies in its slot-0 texture's alpha) and a diffuse texture with real alpha (Material::diffuse_alpha_clear > 0). Then the part is tested + blended like a flagged
// cut-out, even when the loader's alpha_test rule (jboy.h AlphaCutsOut: >= 8 % of texels <= 32) did not mark it. An alpha-free texture, or a variant whose output alpha is not the texture's
// (FX_ALPHA_SPEC / FX_ALPHA_GLOSS: out_alpha 0), is a no-op in the game and stays untested here (a 360 habit, CLAUDE.md rule 6). `plant` = the planted twin of the old rule.
bool YbgTestsAlpha(const assets::Material& mat, const rdr::fx::Variant& ps, bool plant) {
  if (plant || !ps.bytes_alpha || ps.out_alpha != 2 || !(mat.diffuse_alpha_clear > 0.f)) return false;
  return mat.shader.rfind("yBG_", 0) == 0 || mat.shader.rfind("yProp_", 0) == 0;
}
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

// S103 TQF (supersedes the loader's texel rule for the yProp_BumpMap / Default / Reflect class: jboy.h TqfClass): the game tests (GEQUAL 1/255) + blends EVERY yProp model draw (PAS [C stream]); whether that SHOWS depends on the
// variant's output alpha. So a part is tested + blended exactly where its table-picked variant outputs the diffuse alpha (not a gloss technique) and the diffuse texture has alpha, or where its vertex colour alpha is < 255
// (Material::vertex_alpha_lt255: the game blends at vertex alpha x MatAmbCol.w x MatDifCol.w x MdlAlpha, fx_prop.glsl) [L for the vertex term: our port of the game's vertex constants]. Any other class: the B2 rule above.
bool GameTestsAlpha(const assets::Material& mat, int ps_index, int tech_ps, bool plant) {   // tech_ps: the table's variant (FxChoice::tech_ps; = ps_index on the read-rule route), -1 = no effect table: the kept texel verdict decides
  const rdr::fx::Variant& ps = kVariants[size_t(ps_index)];
  if (technique_pick::AppliesTo(read_rule::Lower(mat.shader)) && !technique_pick::PlantOld()) {
    if (plant || !ps.bytes_alpha) return false;
    const fxt::VariantTable* ft = tech_ps >= 0 ? T().by_index[size_t(tech_ps)] : nullptr;
    // the game's colour alpha is the vertex alpha x MatAmbCol.w x MatDifCol.w x MdlAlpha (fx_prop.glsl): any of them < 1 shows in every variant
    const bool colour_alpha = mat.vertex_alpha_lt255 || mat.ambient[3] * mat.diffuse_color[3] < 1.f;
    if (!ft) return (mat.texel_cutout & 1) != 0 || colour_alpha;   // no effect table (a tool without live_assets, a parse failure): the old texel verdict, kept by the loader
    return (technique_pick::ShowsDiffuseAlpha(ft->defines) && mat.diffuse_alpha_clear > 0.f) || colour_alpha;
  }
  if (g_table_pick_all.load(std::memory_order_relaxed) && !technique_pick::PlantOld() && tech_ps >= 0 && technique_pick::IsYDefault(read_rule::Lower(mat.shader))) {   // S103 TQ2: yDefault, the table's variant decides (its defines; the registry's out_alpha is not computed for this effect)
    if (plant || !ps.bytes_alpha || !(mat.diffuse_alpha_clear > 0.f)) return false;
    const fxt::VariantTable* ft = T().by_index[size_t(tech_ps)];
    return ft && technique_pick::YDefaultShowsAlpha(ft->defines);
  }
  return YbgTestsAlpha(mat, ps, plant);
}

bool HasDefine(const fxt::VariantTable& t, const char* define) {
  const std::string all = std::string(" ") + (t.defines ? t.defines : "") + " ";
  return all.find(std::string(" ") + define + " ") != std::string::npos;
}

// S103 RFL: does pixel variant `v` (a registry index) sample the reflection cube (a kind-1 sampler at slot 8)? The yBG_Reflect effects sample it in every variant (their base one carries no FX_REFLECT define), the yProp_ ones where FX_REFLECT is defined.
bool ReadsCube(int v) {
  if (v < 0 || v >= kVariantCount) return false;
  const rdr::fx::Variant& fv = kVariants[size_t(v)];
  for (uint32_t s = 0; s < fv.sampler_count; ++s)
    if (fv.samplers[s].slot == 8 && fv.samplers[s].kind == 1) return true;
  return false;
}

// Options the front-end cannot feed yet (they need a shadow map, a cube map, a second texture layer, a bone palette, a logo ...): a variant
// with one of them is not chosen. The list is the honest edge of version 1 (docs\renderer\SCENE_FRONTEND.md).
bool Unsupported(const fxt::VariantTable& t, bool allow_skinned = false, int allow_reflect = 0 /* S103 RFL: 1 = FX_REFLECT only (the stand-in), 2 = also FX_REFLECT_ADD / _MASK / _MAP (the file cube + amount map are fed) */, bool allow_screen = false, bool allow_vlm = false, int layers = 1, bool allow_planar = false /* S103 TQ2: FX_PLANAR_UV / FX_PLANAR_YZ (the SearchLight vertex variants) */, bool allow_light = false /* S103 RML: FX_LIGHT_SHADOW / FX_SHADOW_LIGHT (the ring mat's props-map pass) */, bool allow_cc_mask = false /* S108 THB: FX_CC_MASK (an entrance prop drawn recoloured: the STS table's variants) */) {   // S101 lane 15: allow_skinned lets FX_SKINNED through; S103 L9: allow_screen = FX_SCREEN_SHADOW (the plane map at slot 12); S103 L10: allow_vlm lets FX_VERTEX_LM / FX_LM_VERTEX through (the page at vertex slot 16)
  static const char* const kBlocked[] = {"FX_SHADOW", "FX_SHADOW_LIGHT", "FX_SCREEN_SHADOW", "FX_LIGHT_SHADOW", "FX_REFLECT", "FX_REFLECT_ADD", "FX_REFLECT_MASK",
                                         "FX_REFLECT_MAP", "FX_CONV_SCREEN", "FX_SCREEN_CONVERT", "FX_EDGE_FADE", "FX_NOISE", "FX_OVERLAY", "FX_LOGO",
                                         "FX_PICKUP", "FX_CAA", "FX_CC_MASK", "FX_SKINNED", "FX_VELOCITY", "VELOCITY", "FX_VERTEX_LM", "FX_SPEC_FADE",
                                         "FX_PLANAR_UV", "FX_PLANAR_YZ", "FX_LAYER2", "FX_LAYER3", "FX_LAYER4", "FX_UV2", "FX_UV3", "FX_UV4",
                                         "FX_LM_VERTEX", "FX_FRAME", "FX_EYE"};
  const int have_layers = HasDefine(t, "FX_LAYER4") || HasDefine(t, "FX_UV4") ? 4 : HasDefine(t, "FX_LAYER3") || HasDefine(t, "FX_UV3") ? 3 : HasDefine(t, "FX_LAYER2") || HasDefine(t, "FX_UV2") ? 2 : 1;   // S105 C2L: yProp_UVScroll spells its layer count FX_UV2 / 3 / 4   // S103 LAY: a UVScroll variant draws exactly as many layers as the material's technique word says (the loader fills Material::layer[])
  if (have_layers != layers) return true;
  for (const char* d : kBlocked)
    if (std::strncmp(d, "FX_LAYER", 8) != 0 && std::strncmp(d, "FX_UV", 5) != 0 && HasDefine(t, d) && !(allow_skinned && std::strcmp(d, "FX_SKINNED") == 0) && !(allow_reflect && std::strcmp(d, "FX_REFLECT") == 0) && !(allow_reflect >= 2 && (std::strcmp(d, "FX_REFLECT_ADD") == 0 || std::strcmp(d, "FX_REFLECT_MASK") == 0 || std::strcmp(d, "FX_REFLECT_MAP") == 0)) && !(allow_screen && std::strcmp(d, "FX_SCREEN_SHADOW") == 0) && !(allow_light && (std::strcmp(d, "FX_LIGHT_SHADOW") == 0 || std::strcmp(d, "FX_SHADOW_LIGHT") == 0)) &&
        !(allow_cc_mask && std::strcmp(d, "FX_CC_MASK") == 0) && !(allow_vlm && (std::strcmp(d, "FX_VERTEX_LM") == 0 || std::strcmp(d, "FX_LM_VERTEX") == 0)) && !(allow_planar && (std::strcmp(d, "FX_PLANAR_UV") == 0 || std::strcmp(d, "FX_PLANAR_YZ") == 0))) return true;  // S101 lane 15 / S103 L9 / S103 L10
  return false;
}

// S103 LAY: how many diffuse layers a UVScroll material draws: the technique word (record +0x90; 0 / 2 / 4 / 6 = UVScroll_1 .. _4, bit 0 = the vertex-LM class: effect_techniques.md)
// says so, capped by the layer textures the file really names (Material::layer, consecutive from layer 1). Every other class: 1.
template <class TexFn>
int UvLayers(const assets::Material& m, const TexFn& tex, bool plant_one_layer) {
  // Only yBG_UVScroll / yBG_UVScrollLM techniques 0..7 are UVScroll_1..4 (+LM) by index (effect_table.py --list); yProp_UVScroll T0..T3 are handled below (S105 C2L), and
  // yBG SearchLight T8..T11 are planar-uv one-pair techniques: those stay at 1 layer here.
  // S105 C2L (C2L.md 3c [C]): yProp_UVScroll T0..T3 = UVScroll_1..4 BY INDEX (effect_table.py --effect yProp_UVScroll: the technique word is the index, no vertex-LM bit): the layers of the file, capped by the layer textures present.
  if (m.shader.rfind("yProp_UVScrol", 0) == 0) {
    if (plant_one_layer || m.technique > 3u) return 1;
    const int want = int(m.technique) + 1;
    int n = 1;
    while (n < want && tex(m.layer[n - 1]) != rdr::kNoImage) ++n;
    return n;
  }
  if (m.shader.rfind("yBG_UVScroll", 0) != 0 || m.technique > 11u) return 1;
  if (plant_one_layer) return 1;
  if (m.technique >= 8u) return (g_table_pick_all.load(std::memory_order_relaxed) && tex(m.layer[0]) != rdr::kNoImage) ? 2 : 1;   // S103 TQ2 (UVS.md): SearchLight_XZ / _YZ (+LM) T8..T11: the table's pair is the two-layer one (no_lm_layer2 pixel), capped by the layer texture present   // S103 LAY planted twin: the old one-layer draw (FrontEndOptions::plant_one_layer)
  const int want = int((m.technique >> 1) & 3u) + 1;
  int n = 1;
  while (n < want && tex(m.layer[n - 1]) != rdr::kNoImage) ++n;
  return n;
}
// S103 TQ2 (UVS.md): the SearchLight techniques of yBG_UVScroll / yBG_UVScrollLM (8 / 9 = SearchLight_XZ(_LM), 10 / 11 = _YZ(_LM)): 1 = planar uv from world x-z, 2 = y-z (FX_PLANAR_YZ), 0 = not a searchlight.
// Only T8 / T10 occur in the player's arenas [C: UVS.md]; the T9 / T11 (+LM) twins carry vertex_lm false in the loader and are asked for the same planar vertex, never seen. Gated on the switch g_table_pick_all (off = the old refusal, exactly).
int SearchLightPlanar(const assets::Material& m) {
  if (!g_table_pick_all.load(std::memory_order_relaxed) || m.shader.rfind("yBG_UVScroll", 0) != 0 || m.technique < 8u || m.technique > 11u) return 0;
  return m.technique >= 10u ? 2 : 1;
}
// S103 L9: the materials whose game draw is the screen-shadow one: the ring mat (yProp_PSM, "Prop Plane Shadow Mat": MAT_SHADOW.md section 1) and its light-map twin.
bool IsPlaneReaderEffect(const std::string& shader) { return shader == "yProp_PSM" || shader == "yProp_PSMLM"; }

// S101 lane 15: the materials the game draws with their light-map ("LM") variant in an entrance (viewer renderer.cpp GpuMaterial::ent_lm; the yBG_* families are off by default).
bool EntranceLmClass(const assets::Material& m, bool bg) {
  if (m.crowd || m.lightmap >= 0) return false;
  std::string lc = m.shader;
  for (char& ch : lc) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return lc == "yprop_default" || lc == "yprop_psm" || (bg && (lc == "ybg_default" || lc == "ybg_uvscroll"));  // (the viewer's ent_lm_bg switch)
}
bool L5PlantNoPage() {  // S103 L5 planted twin (the old behaviour: no page for a match's arena meshes); dev builds only
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_L5_PLANT") != nullptr && std::string(std::getenv("WWE_RDR_L5_PLANT")) != "submodel";   // "submodel" = the rf_c_core hand-over plant only
  return v;
#else
  return false;
#endif
}
bool L10PlantOld() {  // S103 L10 planted twin (the L5 behaviour: the page per pixel for every non-translucent arena mesh in a match); dev builds only
#ifdef RDR_DEV
  static const bool v = std::getenv("WWE_RDR_L10_PLANT") != nullptr;
  return v;
#else
  return false;
#endif
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

// S101 lane 15 / S111 UNIFY5 review: a skinned mesh is cut into skin chunks (each posed by its own palette) when its effect has a SKINNED vertex program in the registry (FX_SKINNED: today the yCh_*,
// yProp_*, yDefault, yAudDefault and yFlashDefault families); any other effect stays rigid at rest (ring_pose.h). Asked of the registry, not a name list: the yProp_* / yDefault list it replaces named the
// same set for every skinned mesh the loaders give (arena, crowd and flash meshes are never skinned: jboy.h `mesh.skinned = !o.arena`). A created superstar's yDefault part is one (the game draws it with
// its table row T1 p1 = yDefault_skinned_vertex: UNIFY5.md review).
bool HasSkinnedProgram(const std::string& effect) {
  for (int i = 0; i < kVariantCount; ++i) {
    const char* n = kVariants[i].name;
    const fxt::VariantTable* t = T().by_index[size_t(i)];
    if (!n || !t || std::strncmp(n, effect.c_str(), effect.size()) != 0 || n[effect.size()] != '_') continue;
    if (EndsWith(n, "_vertex") && HasDefine(*t, "FX_SKINNED")) return true;
  }
  return false;
}

}  // namespace

bool ArenaMatchLm(const std::string& part, const assets::Material& m) {  // S103 L5 (front_end.h)
  int slot = -1;
  if (std::sscanf(part.c_str(), "slot %d", &slot) != 1 || slot < 0 || slot >= 951) return false;
  // S103 L10: in a MATCH the game draws every arena mesh with the plain `yBG_Default` family (run 27 frames 100 / 300 / 500 / 641: slots (0) or (0, 16) only, never slot 13); only the
  // vertex-LM class (record word +0x90 bit 0, Material::vertex_lm) reads the page, in the VERTEX stage (slot 16). The planted twin (WWE_RDR_L10_PLANT) is the old L5 rule: the page per pixel for all.
  return !m.alpha_blend && !IsMovieSurface(part) && !L5PlantNoPage() && EntranceLmClass(m, true);
}
// The classes the stream proves (run 27 frames 100 / 300 / 500 / 641: 105 yBG_Default draws): plain yBG_Default, opaque, no own light map. Everything else keeps its previous rule.
static bool MatchStreamClass(const std::string& part, const assets::Material& m) {
  int slot = -1;
  if (std::sscanf(part.c_str(), "slot %d", &slot) != 1 || slot < 0 || slot >= 951) return false;
  // S103 L13b: a light-map slot that names a stage page texture (Material::lm_stage_page: slots 7 / 10 / 18 / 19 ... of arena 4) is no light map of the mesh's own: the stream draws those meshes with the
  // plain family too (run 28 frame 622: vertex-LM, slots 0 / 16). WWE_RDR_L13B_PLANT=1 (dev) = the old rule (they took the LM pixel pair).
#ifdef RDR_DEV
  static const bool l13b_plant = std::getenv("WWE_RDR_L13B_PLANT") != nullptr;
#else
  constexpr bool l13b_plant = false;
#endif
  const bool own_lm = (m.lightmap >= 0 || m.has_lm_slot) && (l13b_plant || !m.lm_stage_page);
  return m.shader == "yBG_Default" && !m.alpha_blend && !own_lm && !IsMovieSurface(part) && !L5PlantNoPage() && !L10PlantOld();
}
bool ArenaMatchVertexPage(const std::string& part, const assets::Material& m) { return MatchStreamClass(part, m) && m.vertex_lm; }
bool ArenaMatchNoPage(const std::string& part, const assets::Material& m) { return MatchStreamClass(part, m) && !m.vertex_lm; }

// S103 TQF: the registry fragment (of effect `cand`, by name prefix) whose microcode hash is `h`; -1 = none. The D1 read rule's variant_of lookup, strict on the effect (a candidate's own fragments only).
int FragmentOfHash(uint64_t h, const std::string& cand) {
  const auto* first = std::begin(rdr::fx::kHashes);
  const auto* last = std::end(rdr::fx::kHashes);
  const auto* it = std::lower_bound(first, last, h, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
  const std::string prefix = cand + "_";
  for (; it != last && it->hash == h; ++it) {
    const char* n = kVariants[it->variant].name;
    if (n && EndsWith(n, "_fragment") && std::strncmp(n, prefix.c_str(), prefix.size()) == 0) return int(it->variant);
  }
  return -1;
}

// ---- ChooseEffect -------------------------------------------------------------------------------------------------------------

bool& LoosePairPlant() {   // S105 C4 (header)
  static bool v = false;
  return v;
}
bool& WideGuardPlant() {   // S105 C4 fix (header): the FX_SPECULAR guard without `renamed > 0` (the review's F1 regression), the twin of choice_pair_unit
  static bool v = false;
  return v;
}

FxChoice ChooseEffect(const std::string& effect, const MaterialWants& w) {
  FxChoice best;
  best.effect = effect;
  static const bool g_plant_old_choice = std::getenv("WWE_RDR_PLANT_UNLIT_CROWD") != nullptr;   // S103 L4 dev plant (l4_check.py --plant unlit): no FX_LIT bias at all = the old "simplest pair" choice, exactly
  static const bool g_plant_lit_cards = std::getenv("WWE_RDR_PLANT_LIT_CARDS") != nullptr;      // S103 L4 dev plant (--plant lit_cards): the card quad is chosen lit like the bodies
  double best_score = -1e9;
  std::string why;
  std::vector<std::string> candidates;
  if (w.lightmap) candidates.push_back(LmTwin(effect));
  candidates.push_back(effect);
  // S103 TQF: a yProp_BumpMap / Default / Reflect material carries its technique word: the pixel variant is the effect table's row (the game's pick), never the samplers' (technique_pick.h; PAS.md).
  // Every candidate effect (the LM twin too: the game hands the same technique index to the +388 array) is held to its own table row; a candidate with no row is skipped. Nothing bindable -> the retry below, counted.
  const bool reflections = g_reflections.load(std::memory_order_relaxed);   // S103 RFL
  const bool tech_active = w.technique >= 0 && !technique_pick::PlantOld() && (technique_pick::AppliesTo(read_rule::Lower(effect)) || technique_pick::AuditAll() || (reflections && technique_pick::ReflectFamily(read_rule::Lower(effect))) || (g_table_pick_all.load(std::memory_order_relaxed) && technique_pick::AllFamily(read_rule::Lower(effect))));   // S103 TQ2: + yBG_Default / yBG_BumpMap / yDefault
  // S108 THB: an entrance prop drawn recoloured takes the row of the EDIT/STS table (the second effect registry: the game's model+392 row while model+604 is set), the same effect names and technique indices as the startup table, every yProp row a cc_mask variant.
  const std::shared_ptr<const read_rule::EffectTable> tech_table = tech_active ? (w.change_colour ? read_rule::StsEffectTableNow() : read_rule::EffectTableNow()) : nullptr;
  int tech_ps = -1;   // the table's variant of the effect itself (bound or not): the alpha test + blend follows it (GameTestsAlpha), also when only a stand-in can be bound
  if (tech_table) {
    uint64_t ph0 = 0;
    if (technique_pick::PsHash(*tech_table, effect, uint32_t(w.technique), ph0)) tech_ps = FragmentOfHash(ph0, effect);
  }
  // S101 lane 15: the Reflect effects (yBG_Reflect, yProp_Reflect: a vehicle's paint, chrome) read a cube map at slot 8 for their reflection layer. The cube is not fed
  // (it is the stage environment the game bakes at run time): a black cube plus ReflectAlpha = 0 (Resolve: zero) is the do-nothing value, so the surface keeps its diffuse +
  // highlight and has no reflection layer. It is neutral ONLY because ReflectAlpha is 0: FX_REFLECT blends (fx_prop.glsl, fx_bg.glsl), so feeding ReflectAlpha later would
  // darken these surfaces. Counted as a stand-in in the coverage line. Only the plain FX_REFLECT option is taken; the add / map / mask options stay unbound.
  // S103 RFL (RFF.md section A [C]): the game feeds the file cube (sampCubeRefPrm) at slot 8 and the amount map (sampRefMapPrm) at slot 9, so with the switch on the add / mask / map options are bindable
  // (the amount map must exist for a variant that reads slot 9: no map = not bindable = the counted fallback). BindMaterial feeds the cube + ReflectAlpha only to a draw whose pixel variant is the game's own pick.
  // A table-picked material of another effect name (yProp_BumpMap's reflect_reflect_map variants: 144 arena + 31 prop meshes, TQF.md) takes the reflect options too: the hard filter leaves only the table's own variant.
  const int allow_reflect = effect.find("Reflect") != std::string::npos ? (reflections ? 2 : 1) : (reflections && tech_active) ? 2 : 0;
  for (const std::string& cand : candidates) {
    std::vector<int> vertex, fragment;
    Collect(cand, vertex, fragment);
    if (vertex.empty() || fragment.empty()) {
      why = "no effect '" + cand + "' in the registry yet";
      continue;
    }
    const bool is_lm_cand = cand != effect;
    int want_ps = -1;   // S103 TQF: this candidate's table-picked pixel variant (-1 = unrestricted: not a technique-picked draw)
    if (tech_active) {
      uint64_t ph = 0;
      if (!tech_table || !technique_pick::PsHash(*tech_table, cand, uint32_t(w.technique), ph) || (want_ps = FragmentOfHash(ph, cand)) < 0) { why = "technique " + std::to_string(w.technique) + " of '" + cand + "' has no table row / variant"; continue; }
    }
    std::string cand_why = "every variant of '" + cand + "' needs something not fed yet (shadow, cube, second layer, bones ...)";
    for (int f : fragment) {
      if (want_ps >= 0 && f != want_ps) continue;   // S103 TQF: the technique table's pixel variant only
      const fxt::VariantTable& ft = *T().by_index[size_t(f)];
      if (Unsupported(ft, false, allow_reflect, w.screen_shadow, false, w.layers, false, w.light_shadow, w.change_colour)) continue;
      if (w.screen_shadow != HasDefine(ft, "FX_SCREEN_SHADOW")) continue;
      if (w.light_shadow != HasDefine(ft, "FX_LIGHT_SHADOW")) continue;   // S103 RML: the ring mat's props-map pass takes the light_shadow fragment, everything else never does   // S103 L9: the plane reader takes the screen-shadow fragment, everything else never does
      const rdr::fx::Variant& fv = kVariants[f];
      bool slots_ok = true, reads_lm = false;
      for (uint32_t s = 0; s < fv.sampler_count; ++s) {
        const int slot = fv.samplers[s].slot;
        if (slot == 13) { reads_lm = true; slots_ok = slots_ok && w.lightmap; }
        else if (slot == 0 || slot == 6) {}
        else if (slot == 4) slots_ok = slots_ok && w.normal;
        else if (slot == 12 && (w.screen_shadow || w.light_shadow) && fv.samplers[s].kind == 0) {}   // (S103 RML: slot 12 of the light pass = the props map)   // S103 L9: the plane shadow map
        else if (slot == 12 && w.change_colour && HasDefine(ft, "FX_CC_MASK") && fv.samplers[s].kind == 0) {}   // S108 THB: slot 12 of a cc_mask variant = the prop's mask (Material::change_mask[0]); the caller asks for the row only with the mask loaded
        else if (slot == 5) slots_ok = slots_ok && (w.specmap || want_ps >= 0);   // S103 TQF: a table-picked spec-map variant on a material with no spec sampler binds a neutral (black = no highlight) image (BindMaterial); what the game binds there is [U]
        else if (slot >= 1 && slot <= 3 && w.layers > slot) {}   // S103 LAY: the UVScroll layer textures 1..3 (Material::layer)
        else if (slot == 8 && allow_reflect && fv.samplers[s].kind == 1) {}  // the reflection cube (the file's, or the black stand-in)
        else if (slot == 9 && allow_reflect >= 2 && fv.samplers[s].kind == 0) slots_ok = slots_ok && w.refmap;   // S103 RFL: the reflection-amount map (reflect_map / reflect_mask); none = not bindable
        else slots_ok = false;
        if (fv.samplers[s].kind != 0 && !(slot == 8 && allow_reflect && fv.samplers[s].kind == 1)) slots_ok = false;
      }
      if (!slots_ok) continue;
      if (w.alpha_test && !fv.bytes_alpha) continue;
      for (int v : vertex) {
        const fxt::VariantTable& vt = *T().by_index[size_t(v)];
        // S103 L10: an arena LM mesh the game draws with the vertex-LM program (Material::vertex_lm, record word +0x90 bit 0) takes the `*_vertex_lm_vertex` variant (the page at vertex slot 16);
        // every other mesh takes a variant with no vertex texture, as before.
        const bool vlm_pick = w.vertex_lm;
        if (Unsupported(vt, w.skinned, allow_reflect, w.screen_shadow, vlm_pick, w.layers, w.planar != 0, w.light_shadow)) continue;
        if (HasDefine(vt, "FX_SHADOW_LIGHT") != w.light_shadow) continue;   // S103 RML
        if (HasDefine(vt, "FX_SKINNED") != w.skinned) continue;  // S101 lane 15: a skinned prop takes the skinned vertex variant, a rigid one the plain one
        if (HasDefine(vt, "FX_PLANAR_UV") != (w.planar != 0) || HasDefine(vt, "FX_PLANAR_YZ") != (w.planar == 2)) continue;   // S103 TQ2: the planar vertex variant exactly when the material is a SearchLight (XZ / YZ)
        if (vlm_pick != (HasDefine(vt, "FX_VERTEX_LM") || HasDefine(vt, "FX_LM_VERTEX"))) continue;  // S103 L10 (the yAudience spelling is FX_LM_VERTEX: the crowd's page read in the vertex stage)
        bool inputs_ok = true;
        for (uint32_t k = 0; k < vt.input_count; ++k) inputs_ok = inputs_ok && SupportedInput(vt.inputs[k].name, w.skinned);
        if (!inputs_ok) continue;
        if (kVariants[v].sampler_count && !(vlm_pick && kVariants[v].sampler_count == 1 && kVariants[v].samplers[0].slot == 16 && kVariants[v].samplers[0].kind == 0)) continue;  // a vertex texture (the per-vertex light map): only the page at slot 16 for a vertex-LM mesh (S103 L10)
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
        // S103 L4 (docs\systems\crowd.md "Lighting"): the game draws every crowd body / card / prop with the LIT vertex variant (the stream's `yAudience_lit_lm_vertex_vertex`: the four lights + sky / ground
        // ambient at the vertex, then the page). The "simplest pair" rule above picked the unlit `lm_uv` one: the stands came out unshaded (1.5x the game's mean), whatever the light constants were.
        // Only the audience effects have an FX_LIT variant, so this changes no other class; ours reads the page per pixel (`lit_lm_uv`) instead of per vertex (a vertex texture is not fed here).
        // The far-card quad (w.unlit) goes the other way: the game draws cards with yAudience_lm_uv + lm_pixel_color_alpha (CROWD_CARDS_PROMOTION_MAP.md) and its picture is baked lit already.
        if (HasDefine(vt, "FX_LIT") && !g_plant_old_choice) score += (w.unlit && !g_plant_lit_cards) ? -5.0 : 5.0;
        // S105 C4: in fx_bg FX_SPECULAR changes the vertex stage's interpolator layout (normal at 1, eye at 2, colour moves to 3 / 5 / 6: fx_bg.glsl), so a pair must agree on it. The location-only match above let a renamed slot through:
        // yBG_Default_specular_vertex + yBG_Default_base_fragment read the normal as the colour and wrote no alpha (docs\research\s105\C4.md point 3). A pair that disagrees is not a pair; no other pair = the loud fallback below (`why`).
        // `renamed > 0` is the point (review F1): yDefault's layout is the same with or without FX_SPECULAR (its vertex stage never carries it), so a yDefault pair with no renamed slot keeps its pick (32 of 4,864 picks changed without it).
  if (!LoosePairPlant() && (renamed > 0 || WideGuardPlant()) && HasDefine(ft, "FX_SPECULAR") != HasDefine(vt, "FX_SPECULAR")) { cand_why = "no vertex / fragment pair of '" + cand + "' agrees on FX_SPECULAR (the interpolator layout differs)"; continue; }
        const bool has_spec = HasDefine(ft, "FX_SPECULAR") || HasDefine(ft, "FX_SPEC") || HasDefine(vt, "FX_SPECULAR") || HasDefine(vt, "FX_SPEC");
        score += has_spec ? (w.spec ? 4 : -4) : (w.spec ? -1 : 0);
        if (HasDefine(ft, "FX_SPEC_MAP") && w.specmap) score += 2;
        // S101 lane 257: an arena / prop material with a highlight and no specular map keeps its gloss mask in the diffuse alpha (the viewer rule, viewer_hlsl.h "arena highlight:
        // strength in the diffuse alpha"; jboy.h: floor / stair / rope mats have alpha 60-110 everywhere, a mask, not a cut-out). The FX_ALPHA_SPEC pair multiplies the highlight by it.
        // Without this the plain FX_SPECULAR pair was chosen: a full-strength lobe (stand floors: level 0.5, power 5) that washed floors and curtains white (census tile faults).
        if (HasDefine(ft, "FX_ALPHA_SPEC")) score += (w.spec && !w.specmap && !w.alpha_test) ? 3.0 : -3.0;
        if (HasDefine(ft, "FX_ALPHA_GLOSS") && w.alpha_mask) score += (w.spec && !w.specmap) ? 3.0 : -3.0;  // S103 P1: the yProp spelling of the same pair (arena slots 390-393 copies: the game's variant is spec_alpha_gloss)
        if (w.tech_shows >= 0) score += (technique_pick::ShowsDiffuseAlpha(ft.defines) == (w.tech_shows == 1)) ? 20.0 : -20.0;   // S103 TQF: a stand-in for an unbindable table variant must agree with it on whether the diffuse alpha shows (a gloss technique drawn with a spec_map stand-in went see-through)
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
  if (!best.Ok() && tech_active && w.change_colour) {  // S111 BELTMASKNAME review: the EDIT/STS table has no bindable row for this prop (the m2 belts' yProp_Default T0): the plain draw is the material's own pick WITHOUT the recolour (the startup table's row), never the sampler stand-in below (stockbelt_unit real.cbwm.no_row_draws_plain_pick)
    MaterialWants plain = w;
    plain.change_colour = false;
    return ChooseEffect(effect, plain);
  }
  if (!best.Ok() && tech_active) {  // S103 TQF: the table's variant is not bindable for this material (a spec-map technique on a material with no spec sampler, an input not fed ...) or not in the table: the sampler scoring, counted (docs\research\s103\TQF.md)
    MaterialWants plain = w;
    plain.technique = -1;
    plain.change_colour = false;   // S108 THB: the STS row was not bindable: the stand-in is the plain draw (the caller counts the missing row)
    plain.tech_shows = (tech_ps >= 0 && T().by_index[size_t(tech_ps)]) ? int(technique_pick::ShowsDiffuseAlpha(T().by_index[size_t(tech_ps)]->defines)) : -1;
    FxChoice fb = ChooseEffect(effect, plain);
    fb.tech_fallback = true;
    fb.tech_ps = tech_ps;
    return fb;
  }
  if (best.Ok() && tech_active) best.tech_picked = true, best.tech_ps = tech_ps;
  if (best.Ok() && tech_active && w.change_colour && T().by_index[size_t(best.ps)] && HasDefine(*T().by_index[size_t(best.ps)], "FX_CC_MASK")) best.cc_row = true;   // S108 THB
  if (!best.Ok() && w.vertex_lm) {  // S103 L10: no vertex-LM variant for this effect + pixel pair: retry with the per-pixel page pair (the L4 / L5 choice) when the page exists, else plain; never the debug colour
    MaterialWants plain = w;
    plain.vertex_lm = false;
    plain.lightmap = w.page_available;
    FxChoice fb = ChooseEffect(effect, plain);
    fb.vlm_fallback = true;
    return fb;
  }
  if (!best.Ok() && w.layers > 1) {  // S103 LAY backstop: no multi-layer variant fits (an unforeseen technique class): the one-layer pick, counted, never the debug colour
    MaterialWants one = w;
    one.layers = 1;
    FxChoice fb = ChooseEffect(effect, one);
    fb.layer_fallback = true;
    return fb;
  }
  if (!best.Ok()) {
    best.vs = best.ps = -1;
    best.effect = effect;
    best.why_not = why.empty() ? "no variant" : why;
  }
  return best;
}

// S103 B1: the game's sampler state for (effect, stage), from the player's own effect files (sampler_table.h, T1_samplers.md). The effect's stage array is found ONCE per bound material
// (SamplerSlot, in Bound); per draw a slot costs one array read and, for the setting, one relaxed atomic read (AnisoWanted). A hit replaces the address / filter / mip fields the caller
// filled with today's heuristics; a miss keeps them. `map_slot` = the slot holds one of our own frame maps (the shadow / plane map: single-level): only there is the table's point mip
// taken, and it stays 1x. The d+28 per-parameter address override (sub_82C04DF0) is not in our draw data: the pass value stays (the compare tool saw no address mismatch).
struct SamplerSlot {
  const sampler_table::EffectSamplers* es = nullptr;
  bool done = false;   // resolved against a published table (a null `es` then = this effect has no sampler block)
};
static void TableSampler(SamplerSlot& ss, const FxChoice& c, int slot, bool map_slot, bool vertex, rdr::SamplerDesc& sd) {
  namespace st = sampler_table;
  if (st::g_plant_old_samplers) return;   // rig-only twin: the old rules (no table, fixed 8x)
  if (!ss.done) {
    if (!st::Now()) return;   // no table published (a rig without the player's files): the old heuristics stay
    ss.es = st::Find(c.effect);
    ss.done = true;
  }
  if (ss.es && slot >= 0 && slot < st::kStages && ss.es->st[size_t(slot)].set) {
    st::Apply(ss.es->st[size_t(slot)], sd, map_slot);
    if (vertex) sd.max_anisotropy = 1;
  }
  if (map_slot) sd.mip_mode = 0, sd.max_anisotropy = 1;   // slot 12: the frame's map, single-level: point mip [C census: every pass of every effect that has it]
}

// ---- per-model data -----------------------------------------------------------------------------------------------------------

struct FrontEnd::Bound {
  FxChoice choice;
  mutable SamplerSlot samp;   // S103 B1: the effect's sampler stages, resolved on first draw

  bool debug = false;
  uint16_t program = 0, layout = 0;
  rdr::ImageId slot_image[rdr::kMaxTextures] = {};
  uint8_t slot_clamp[rdr::kMaxTextures] = {};
  bool alpha_test = false;
  mutable uint16_t zw_plain = 0;                          // S105 SZW (TEST ONLY plant): the pair's program WITHOUT the alpha twin
  mutable uint16_t zw_program = 0;                        // S105 SZW: the alpha-twin program of the bound pair (the discard the game's GEQUAL 1/255 stage test needs); set for a stage-slot / screen-piece material (Material::stage_zw) whose pixel variant HAS a twin, and (S108 HAIR) for a piece-cut draw's pair that has one
  mutable bool pc_notwin_reported = false;                // S108 HAIR: the piece_cut_notwin draw_shortfall of this pair was written (once per pair)
  bool zw_twin = false;                           // S105 SZW: zw_program is valid (no twin = the draw cannot discard = it writes no depth under wwe2k14_rdr_stage_zwrite)
  bool no_table = false;                          // S103 TQF: a technique-class material bound while no effect table existed (the sampler scoring + texel verdict): BindMaterial rebuilds it once the table is there
  bool cube_real = false;                         // S103 RFL: slot 8 holds the model's own file cube and the material has a g_fReflectAlpha > 0: Resolve feeds ReflectAlpha (else 0 = the black stand-in's do-nothing value)
  bool vtx_lm = false;                           // S103 L10: the vertex variant reads the page at vertex slot 16 (slot_image[16])
  bool shadow = false;                            // S103 L6: bound to the shadow-reader variant (slot 12 = the frame's shadow map)
  bool plane = false;                             // S103 L9: bound to the plane-shadow reader (FX_SCREEN_SHADOW, slot 12 = the frame's plane map)
  bool props_reader = false;                      // S103 RML: the ring mat bound to the game's props-map pass (FX_LIGHT_SHADOW + FX_SHADOW_LIGHT, slot 12 = the props map)
  bool chr = false, hair = false;                 // S101 lane 13: a yCh_* material (the character path); hair / translucent: drawn twice
  bool cube_analytic = false;                     // S103 YCB (review F3): the table pick bound a cube row's non-cube twin (CharCtx::cube_analytic: lobe power 0 -> 1 as the bake)
  bool char_table = false;                  // S103 YCB: a wrestler pair that is the game's table row for (technique, pass) (the draw then skips the old shadow / wet pairs)
  bool reads_wrinkles = false, reads_idmap = false;   // S103 XLF: the bound pixel variant is a "_wrinkles" one / the vertex variant an "id_map" one (decided once at the bind, not by strstr per draw)
  bool wrk_fields = false;                        // S103 WRK (review F4): the bound vertex variant declares WklLvLst / IdMapLvLst (the levels are joined and stamped only for these)
  bool wet = false;                               // S103 SWF: bound to the wet skin variant (CharCtx::wet: the material's wet constants are fed)
  uint8_t liquid_slot = 0xFF;                     // S103 SWF: the sampler slot (14) that holds the wrestler's liquid map, set at the draw; 0xFF = none
  bool change_colour = false;                     // S108 THR2: bound to the STS (Threads editor) row with the masks at 7 / 4 / 14 / 2: the draw feeds ChangeColor (CharCtx::change_color)
  uint8_t cube_slot[rdr::kMaxTextures] = {};      // 1 = this sampler slot holds the object's ambient cube (13), 2 = a screen-space map (clamped), 4 = the frame's shadow map (S103 L6), 5 = the frame's plane shadow map (S103 L9)
};

struct FrontEnd::ModelData {
  std::string label;
  std::vector<rdr::ImageId> images;
  bool images_external = false;     // S103 lane D: the caller owns the images (AddModel's `shared`)
  bool hook_owned = false;          // S103 lane D: drawn by a stage hook only
  uint8_t group = rdr::kGrpOther;   // S103 GSTP: the GPU group stamps' group of this model's draws (rdr::GroupId): SetModelGroup by the owner, else the label rule (model_group.h GroupOfLabel)
  bool unlit = false;               // S103 L4: the unlit audience variant (the far-card quad)
  struct ImgInfo { uint32_t w = 0, h = 0, mips = 0; bool rgba8 = false; int fmt = -1; uint32_t faces = 1; };   // S103 RFL: faces 6 = a cube image (the Reflect effects' sampCubeRefPrm)  // S103 CAWS: fmt = assets::TexFormat of the source (the addmat line names the picture)  // S101 lane 15: what UpdateTexture may replace
  std::vector<ImgInfo> img_info;
  // S103 PVR: a model whose vertices are re-posed every frame (UpdateModelVertices, the crowd's bodies) keeps the latest posed block on the CPU (the whole vb layout, `vb_bytes`) and, with g_posed_ring on, pushes it into
  // each frame's geometry ring on its first draw of that frame. `posed_serial` counts updates (0 = never posed: draw from `vb`), `vb_serial` = the update `vb` holds (the buffer path / the overflow fallback).
  std::vector<uint8_t> posed_cpu;
  uint32_t vb_bytes = 0, ring_base = 0;
  uint64_t posed_serial = 0, vb_serial = 0, ring_epoch = 0, ring_serial = 0, seen_epoch = 0, seen_serial = 0;   // ring_serial: the update the pushed block holds; seen_*: the at-rest count
  // S103 PF2: the direct path's copies (kPosedCopies host-visible mapped vertex buffers of vb_bytes each, made on the first re-pose). pcopy_cur = the copy that holds the latest pose (-1 = none: draw from `vb`, the
  // ring or the fallback's staged write); pcopy_use[i] = the newest Render index that may read copy i (RenderSerial() + 1 when written or drawn: the tween's second render of a tick is one more).
  std::vector<rdr::BufferId> pcopy;
  std::vector<uint8_t*> pcopy_ptr;
  std::vector<uint64_t> pcopy_use;
  int pcopy_cur = -1;                 // INVARIANT (S103 PF2): `vb` is read ONLY through FrontEnd::PosedVertexBuffer (tools\scene\posed_vb_lint.py). pcopy_cur >= 0: the latest pose is in that copy and `vb` is STALE; pcopy_cur < 0: `vb` holds
                                      // the latest pose (the as-added one, the staged fallback's write, or the write that released the copies) or the ring holds it (posed_serial / vb_serial)
  uint64_t pcopy_pose = 0;            // the Render serial of the last write into a copy (the at-rest sweep frees the copies of a model not re-posed for kPosedRestRenders)
  bool pcopy_failed = false;          // the copies could not be made: this model stays on the ring path
  uint32_t still = 0;                // frames this model was drawn with the same posed_serial (at rest: written into `vb` once, drawn from it)
  struct Mesh {
    uint32_t vb_offset = 0, ib_offset = 0, index_count = 0, vertex_count = 0;
    int material = -1, placement = -1, light_set = -1;
    bool skinned = false;
    float center[3] = {0, 0, 0};
    std::string part;
    int src = 0;                      // S101 lane 13: the ordinal of the source mesh (Object::mesh_mask bits)
    int slot_no = -1;                 // S103 E5: the stage slot of an arena mesh ("slot N"), -1 = not one
    int src_record = -1;              // S103 E5: the mesh record's index in its JBOY = the game's draw part (assets::Mesh::src_record)
    std::string node, src_pac;        // S103 CAWS: the source mesh's first node name / pac (assets::Mesh::node / src_pac), for the [scene][blended] line
    bool answers_piece_row = false;   // S105 A5b F4: MeshAnswersPieceRow(src_kind) (S111 GUESTKEY: no pac test), decided once at AddModelImpl (the Record loop runs per mesh per frame)
    bool piece_cut = false;           // S108 HAIR: piece_draw::PieceCut (a stock piece's translucent mesh outside sub_8259E488's ref-1 list): alpha test GEQUAL 128/255 + depth write, blended; decided once at AddModelImpl
    int src_kind = 0, src_key = -1;   // S103 PZF2 (EB): the source mesh's pac entry kind / key (assets::Mesh::src_kind / src_key): kind 2 + key = a wrestler's piece id (0 body, 10000+ hair ...)
    bool uv_scroll = false;           // S103 E5: its material's shader is an UV-scroll class (reads VS c186 Time)
    bool lid_layer = false;           // S103 EYF: a wrestler's eyelid / lash layer (piece 20000-39999, a `trans` effect, a `mabuta` palette), the viewer's rule (view\renderer.cpp lid_layer)
    bool cloth = false;               // a skinned cloth piece: posed on the CPU every frame (the effect does not skin its position)
    std::vector<assets::Vertex> cpu;  // ... its bind vertices
    bool chunk = false;               // a skin chunk of a character mesh: the second vertex stream below and `nodes` are valid
    uint32_t sv_offset = 0;           // into ModelData::svb
    std::vector<uint16_t> nodes;      // palette slot -> skeleton node
    std::vector<uint32_t> extra_pieces;   // S103 W2B: the piece ids whose extra nodes (Bone::piece_id) `nodes` includes (ascending order of first use); empty = the chunk skins to the body's nodes only
  };
  std::vector<Mesh> meshes;
  std::vector<assets::Material> materials;
  std::vector<std::vector<assets::Matrix>> placements;
  std::vector<uint8_t> stage_cull;  // S103 V1: by stage slot, bit 1 base copy hidden, bit 2 mirrored / turned copy hidden (SetStageCull)
  uint16_t idle_screens = 0;        // S103 SCF: bit k = the movie-key slot 800 + k is shown and plays no movie this frame: it draws the live frame grab (live_screen_), the game's sub_8276A808 (SetIdleScreens)
  std::vector<assets::LightSet> light_sets;
  uint32_t node_count = 0;  // S103 B: the model's skeleton nodes (assets::Model::bones), for ModelNodeCount
  bool skinned_model = false;   // S111 OLDCHAIN review: the model has a skin chunk (a skinned character model: the game's model +540 == 1, technique_pick.h CharPassOf's base); set at AddModelImpl, never from a frame's palette
  uint32_t main_node_count = 0;                           // S103 W2B: the body entry's own nodes (0 = the model has no piece nodes); node_count - main_node_count = extra_tags.size()
  std::vector<uint32_t> extra_piece_nodes;                // S103 W2B (review G): parallel to extra_tags: the piece entry's node count
  std::vector<FrontEnd::ModelExtraInfo> extra_info;       // S103 W2B (review D / F): parallel to extra_tags
  std::vector<std::pair<uint32_t, uint32_t>> extra_tags;  // S103 W2B: (piece id, node index in the piece) of every bone after the body's, in model order (= canonical: piece id ascending, then node)
  float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};   // S103 CAWB: assets::Model::bounds_min / max, for ModelBounds
  int stage_lm = -1;
  std::vector<std::pair<int, assets::Model::SlotRecord>> slot_records;   // S103 D1: the arena's slot records (sorted by slot), from the loader
  int arena_id = -1;                                                     // S103 D1: "arena N" of the label ([stage+1216] = the arena id [L], R10 class4)
  std::unordered_map<uint64_t, std::unique_ptr<Bound>> rule_bound;      // S103 D1: BindRule's cache (material, skinned, vs, ps, shadow)
  std::unordered_map<uint64_t, const Bound*> rule_pick;                 // S103 D1: RuleFor's answer per (mesh, phase == 1, shadow map present): the rule runs once per mesh and look, not per draw
  // S103 B3: lm_xy / lm_gain = c184 of this model (x y = 1 / span, z = gain): the arena's own value from the game's table (arena_lm.h, set at load). lm_base_gain = the table's gain; a card-technique
  // model (lm_card: the crowd card quad) draws with the gain x the card factor of the frame (ApplyLmRow: the game's [0x8376383C] * 0.001, only when > 0).
  float lm_xy[2] = {1.f / 3500.f, 1.f / 3500.f}, lm_gain = 2.5f;
  float lm_base_gain = 2.5f;
  bool lm_card = false;
  std::unordered_map<uint64_t, rdr::BufferId> cloth_bufs;  // posed cloth vertices, by (object, mesh)
  rdr::BufferId vb = rdr::kNoBuffer, ib = rdr::kNoBuffer, svb = rdr::kNoBuffer;  // (svb: the skin stream of the character chunks)
  std::vector<std::unique_ptr<Bound>> bound;  // per material (x2: rigid / skinned, S101 lane 13)
  std::vector<uint8_t> char_class;   // S103 YCB (review P2-5): per material, bit 0 = computed, 1 = E+12, 2 = E+4, 3 = hair, 4 = E+20 (S111 TECHPASS) (technique_pick.h CharClassOf)
  std::unordered_map<uint64_t, std::unique_ptr<Bound>> char_bound;   // S103 YCB: the wrestler table pick's cache, by (material, skinned, pass) (BindMaterial's char_pass); cleared with `bound`
  bool live = true;
  bool images_shared = false;  // S103 R: the images belong to another model (AddModelSharing): ReleaseModel leaves them
  int page_tex = -1;           // S103 UP: the texture slot that is the group's shared page image (AdoptPage); -1 = the model's page, if any, is its own
  uint64_t page_group = 0;     // S103 UP: the page group (ArenaSet) whose image that slot holds; ReleaseModel gives the reference back instead of destroying it
  rdr::ImageId page_orig = rdr::kNoImage;   // S103 UP: a BORROWED model's own page image while it sits on the group's (LeavePage puts it back)
  // S103 UPL U2: the screens' picture images. A pool of RGBA8 images of the screens' sizes: each live screen texture shows ONE pool image (scr_of[texture], -1 = not adopted yet; images[texture] holds it);
  // `users` = the textures showing it; `sent` = the bytes it holds (known), so an equal picture is found without an upload. A texture's own image joins the pool when it is first updated.
  struct ScrImg { rdr::ImageId image = rdr::kNoImage; uint32_t w = 0, h = 0; int users = 0; bool known = false; uint64_t id = 0; };   // id = the content id of the picture it holds (known); no bytes are kept
  std::vector<ScrImg> scr_pool;
  std::vector<int> scr_of;
  std::vector<int64_t> scr_moved_at;   // per texture: the batch number of its last re-bind (a screen that just moved is not moved again for a one-frame serial lag)
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
  if (grab_prime_ != rdr::kNoImage) renderer_.DestroyImage(grab_prime_), grab_prime_ = rdr::kNoImage;   // WGS PAR1: the TEST-ONLY primed grab (teardown)
  for (auto& kv : cubes_)
    if (kv.second.image != rdr::kNoImage) renderer_.DestroyImage(kv.second.image);
}

FrontEnd::ModelData& FrontEnd::Mod(int id) { return *models_[size_t(id)]; }

static void MeshCenter(const assets::Mesh& me, float center[3]);   // S103 PF2

// S103 R: rewrite a static model's vertex buffer IN PLACE from `model` (the same meshes, the same vertex counts, re-posed): the ring pieces and posed weapons (ModelCache::PosedModel; S103 PVR: never the crowd). The upload goes
// through the renderer's batched UpdateBuffer, ordered with the frames. False (nothing written) when the shape differs or the model has skin chunks / cloth.
bool FrontEnd::UpdateModelVertices(int id, const assets::Model& model) {
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return false;
  ModelData& m = Mod(id);
  if (m.vb == rdr::kNoBuffer) return false;
  const auto skipped = [](const assets::Mesh& me) { return me.vertices.empty() || me.indices.size() < 3 || me.part.rfind("crowd flash", 0) == 0; };
  size_t pi = 0;
  for (const assets::Mesh& me : model.meshes) {
    if (skipped(me)) continue;
    if (pi >= m.meshes.size()) return false;
    const ModelData::Mesh& mm = m.meshes[pi];
    if (mm.chunk || mm.cloth || mm.vertex_count != me.vertices.size()) return false;
    ++pi;
  }
  if (pi != m.meshes.size()) return false;
  pi = 0;
  bool ring = g_posed_ring.load(std::memory_order_relaxed);   // S103 PVR: the posed block is kept here and pushed into the frame's geometry ring at the first draw; the old path copies it into the model's buffer now
  if (ring && PosedDirectOn() && !m.pcopy_failed) {   // S103 PF2: a unified-memory device writes the pose straight into a mapped copy of the buffer (one copy of the bytes)
    if (PosedDirectWrite(m, model)) return true;
    if (!m.pcopy_failed) ring = false;   // every copy is still in flight (counted): the staged write into the model's own buffer, as the old path did
  }
  m.pcopy_cur = -1;   // (PF2: not drawn from a copy any more)
  if (ring) {
    if (m.posed_cpu.size() != m.vb_bytes) m.posed_cpu.assign(m.vb_bytes, 0);
    ++m.posed_serial;
  }
  for (const assets::Mesh& me : model.meshes) {
    if (skipped(me)) continue;
    ModelData::Mesh& mm = m.meshes[pi++];
    if (ring) {
      const size_t bytes = me.vertices.size() * sizeof(assets::Vertex);
      if (size_t(mm.vb_offset) + bytes <= m.posed_cpu.size()) std::memcpy(m.posed_cpu.data() + mm.vb_offset, me.vertices.data(), bytes);
    } else {
      renderer_.UpdateBuffer(m.vb, mm.vb_offset, me.vertices.data(), me.vertices.size() * sizeof(assets::Vertex), rdr::kBufPosed);
    }
    MeshCenter(me, mm.center);
  }
  if (!ring) m.posed_serial = m.vb_serial = 0;   // the buffer holds the latest (and a ring-mode history is forgotten: draws read `vb`)
  return true;
}

static void MeshCenter(const assets::Mesh& me, float center[3]) {   // S103 PF2: the bounds centre of a posed mesh (was inline in UpdateModelVertices)
  float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
  for (const assets::Vertex& v : me.vertices)
    for (int k = 0; k < 3; ++k)
      if (std::isfinite(v.pos[k])) mn[k] = std::min(mn[k], v.pos[k]), mx[k] = std::max(mx[k], v.pos[k]);
  for (int k = 0; k < 3; ++k) center[k] = mn[k] <= mx[k] ? (mn[k] + mx[k]) * 0.5f : 0.f;
}

bool FrontEnd::PosedDirectOn() const {   // S103 PF2
  return g_posed_ring.load(std::memory_order_relaxed) && g_posed_direct.load(std::memory_order_relaxed) && (g_posed_direct_force.load(std::memory_order_relaxed) || renderer_.UnifiedMemory());
}

// S103 PF2: the skinned vertices of every mesh straight into the next free mapped copy of the model's vertex buffer. `model` has been shape-checked by UpdateModelVertices (same meshes, same vertex counts).
// A copy is free when no Render that may still be on the GPU reads it: its newest reader index (pcopy_use, RenderSerial() + 1 at its last write / draw: this tick's second render of the tween is one more) is below
// RenderSerial() - FramesInFlight() (Render s waits for frame s - FramesInFlight() before it records). False: no copy free (counted: the staged fallback), or the copies could not be made (pcopy_failed).
bool FrontEnd::PosedDirectWrite(ModelData& m, const assets::Model& model) {
  const int plant = g_plant_posed_direct.load(std::memory_order_relaxed);
  if (m.pcopy.empty()) {
    const uint32_t want = (plant == 5 || plant == 6) ? 1u : kPosedCopies;
    if (m.vb_bytes == 0) return m.pcopy_failed = true, false;
    for (uint32_t i = 0; i < want; ++i) {
      uint8_t* p = nullptr;
      const rdr::BufferId id = renderer_.CreateHostBuffer(m.vb_bytes, &p);
      if (id == rdr::kNoBuffer || !p) {
        for (rdr::BufferId b : m.pcopy) renderer_.DestroyBuffer(b);
        m.pcopy.clear(), m.pcopy_ptr.clear(), m.pcopy_use.clear();
        return m.pcopy_failed = true, false;
      }
      m.pcopy.push_back(id), m.pcopy_ptr.push_back(p), m.pcopy_use.push_back(0);
    }
    g_posed_direct_made.fetch_add(want, std::memory_order_relaxed);
  }
  const uint64_t serial = renderer_.RenderSerial();
  const uint64_t fif = renderer_.FramesInFlight();
  const uint64_t done = serial > fif ? serial - fif : 0;   // Renders with an index below this have finished on the GPU
  const size_t cnt = m.pcopy.size(), pref = size_t((m.posed_serial + 1) % cnt);
  const bool check = plant != 6;   // (plant 6, rig only: the in-flight check is left out)
  int pick = -1;
  for (size_t k = 0; k < cnt && pick < 0; ++k) {
    const size_t i = (pref + k) % cnt;
    if (!check || ((m.pcopy_use[i] == 0 || m.pcopy_use[i] < done) && renderer_.HostBufferIdle(m.pcopy[i]))) pick = int(i);   // free only when the front end's mark AND the renderer's own reader record agree
  }
  if (pick < 0) {
    g_posed_direct_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!renderer_.HostBufferIdle(m.pcopy[size_t(pick)])) g_posed_direct_inflight.fetch_add(1, std::memory_order_relaxed);   // the audit, from the renderer's own record (last_read), independent of the front end's marks: never true with the check on
  uint8_t* dst = m.pcopy_ptr[size_t(pick)];
  size_t pi = 0, wrote = 0;
  for (const assets::Mesh& me : model.meshes) {
    if (me.vertices.empty() || me.indices.size() < 3 || me.part.rfind("crowd flash", 0) == 0) continue;   // (UpdateModelVertices' skipped(): the meshes the model never drew)
    ModelData::Mesh& mm = m.meshes[pi++];
    const size_t bytes = me.vertices.size() * sizeof(assets::Vertex);
    if (size_t(mm.vb_offset) + bytes <= m.vb_bytes) std::memcpy(dst + mm.vb_offset, me.vertices.data(), bytes), wrote += bytes;
    MeshCenter(me, mm.center);
    g_posed_direct_writes.fetch_add(1, std::memory_order_relaxed);
  }
  g_posed_direct_bytes.fetch_add(wrote, std::memory_order_relaxed);
  ++m.posed_serial;
  m.pcopy_cur = pick;
  m.pcopy_use[size_t(pick)] = serial + (rdr::kRendersPerRecord - 1);   // the newest Render that may read it: this tick's last one
  m.pcopy_pose = serial;
  return true;
}

rdr::BufferId FrontEnd::PosedVertexBuffer(ModelData& m) {   // S103 PF2
  if (m.pcopy_cur < 0) return m.vb;
  size_t i = size_t(m.pcopy_cur);
  m.pcopy_use[i] = renderer_.RenderSerial() + (rdr::kRendersPerRecord - 1);   // this tick's Renders (the tween's two) read it
  if (g_plant_posed_direct.load(std::memory_order_relaxed) == 7) i = (i + 1) % m.pcopy.size();   // PLANT 7 (rig only): the copy after the one written: an older pose
  return m.pcopy[i];
}

// S103 PF2: a model whose copies were last written kPosedRestRenders Renders ago (at rest) frees them: ONE staged write of the latest pose into `vb` (the old path's write: ordered with the frames), pcopy_cur = -1, the
// copies destroyed (the renderer frees them 12 frames later: no frame in flight loses them). The next re-pose makes them again.
void FrontEnd::SweepPosedCopies() {
  const uint64_t serial = renderer_.RenderSerial();
  for (ModelData* mp : models_) {
    ModelData& m = *mp;
    if (!m.live || m.pcopy.empty()) continue;
    if (serial < m.pcopy_pose + kPosedRestRenders) continue;   // (at rest = no re-pose; the model is still drawn, now from `vb`)
    if (m.pcopy_cur >= 0) renderer_.UpdateBuffer(m.vb, 0, m.pcopy_ptr[size_t(m.pcopy_cur)], m.vb_bytes, rdr::kBufPosed);
    for (rdr::BufferId b : m.pcopy) renderer_.DestroyBuffer(b);
    m.pcopy.clear(), m.pcopy_ptr.clear(), m.pcopy_use.clear(), m.pcopy_cur = -1;
    m.posed_serial = m.vb_serial = 0;   // draws read `vb` (the ring state is forgotten)
    g_posed_direct_released.fetch_add(1, std::memory_order_relaxed);
  }
}

void FrontEnd::NotePosedCensus() {   // S103 PF2: the census of the posed vertices: one line every 600 Records
  ++posed_census_frames_;
  if (posed_census_frames_ % 60 == 0) SweepPosedCopies();
  if (posed_census_frames_ < 600) return;
  posed_census_frames_ = 0;
  const uint64_t now[6] = {g_posed_direct_writes.load(), g_posed_direct_bytes.load(), g_posed_direct_fallbacks.load(), g_posed_ring_pushes.load(), g_posed_ring_bytes.load(), g_posed_ring_overflow.load()};
  uint64_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = now[i] - posed_census_[i], posed_census_[i] = now[i];
  if (!(d[0] | d[2] | d[3] | d[5])) return;   // nothing posed in the window
  const double mb = 1.0 / 1048576.0;
  rdr::Log("[scene][posed] last 600 Records, a frame: mapped writes %.1f meshes (%.2f MB) straight into the model's host-visible copy; ring pushes %.1f (%.2f MB, overflow %.2f); copy-in-flight fallbacks to the staged copy %.3f; copies made %llu in all, freed at rest %llu; in-flight writes (audit, 0 = right) %llu",
           double(d[0]) / 600.0, double(d[1]) * mb / 600.0, double(d[3]) / 600.0, double(d[4]) * mb / 600.0, double(d[5]) / 600.0, double(d[2]) / 600.0,
           (unsigned long long)g_posed_direct_made.load(), (unsigned long long)g_posed_direct_released.load(), (unsigned long long)g_posed_direct_inflight.load());
}

// S103 PVR: one push into the frame's geometry ring under the posed budget. Two limits: kPosedRingCap posed bytes a frame, and the renderer's REAL room (a frame over the ring is dropped whole): what is
// already pushed + this block + what the other producers (crowd, particles, map plane, post) are expected to push still (their size in recent frames) + the constants (twice: the previous tick's copy) + 1 MB.
// False (counted) = the caller uses the buffer path for this mesh.
bool FrontEnd::PosedPush(rdr::FrameBuilder& fb, const void* data, uint32_t bytes, uint32_t& offset) {
  const uint64_t ep = fb.Epoch();
  if (posed_epoch_ != ep) posed_epoch_ = ep, posed_bytes_ = 0;
  const uint64_t used = fb.GeometryBytes(), room = renderer_.RingRoom();
  const uint64_t others_now = used - std::min<uint64_t>(used, posed_bytes_);
  const uint64_t expect = uint64_t(other_est_) + other_est_ / 4;
  const uint64_t later = expect > others_now ? expect - others_now : 0;
  const uint64_t consts = 2ull * std::max<uint32_t>(fb.ConstantsBytes(), const_est_);
  if (uint64_t(posed_bytes_) + bytes > kPosedRingCap || (used + bytes + 16 + later + consts + (1ull << 20) > room && g_plant_posed_ring.load(std::memory_order_relaxed) != 4)) {   // (PLANT 4, rig only: the room check is left out)
    g_posed_ring_overflow.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  offset = fb.PushGeometry(data, bytes, 16);
  posed_bytes_ += bytes;
  g_posed_ring_pushes.fetch_add(1, std::memory_order_relaxed);
  g_posed_ring_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

void FrontEnd::NoteRingFrame(const rdr::FrameBuilder& fb) {   // the end of a Record: every producer of the frame has pushed
  NotePosedCensus();   // S103 PF2
  const uint32_t used = fb.GeometryBytes();
  const uint32_t others = used - std::min<uint32_t>(used, posed_epoch_ == fb.Epoch() ? posed_bytes_ : 0u);
  other_est_ = std::max(others, other_est_ - other_est_ / 8);   // the biggest of the last frames, easing down
  const_est_ = std::max(fb.ConstantsBytes(), const_est_ - const_est_ / 8);
}

bool FrontEnd::PosedRingBase(rdr::FrameBuilder& fb, ModelData& m, uint32_t& base) {
  if (m.posed_serial == 0 || !g_posed_ring.load(std::memory_order_relaxed)) return false;   // never posed (or the old path): the buffer holds the vertices
  if (m.pcopy_cur >= 0) return false;   // S103 PF2: the model's latest pose is in a mapped copy (PosedVertexBuffer): no ring push
  if (m.vb_serial == m.posed_serial) return false;   // at rest (or the ring had no room this frame): the model's own buffer holds the latest block
  const uint64_t ep = fb.Epoch();
  if (ep != 0 && m.ring_epoch == ep && m.ring_serial == m.posed_serial) { base = m.ring_base; return true; }   // pushed already this frame (another pass / object of the same model), and no re-pose since
  const int plant = g_plant_posed_ring.load(std::memory_order_relaxed);
  if (plant == 2 && m.ring_epoch != 0) {   // PLANT 2 (rig only): the previous frame's offset, nothing pushed
    m.ring_epoch = ep, m.ring_serial = m.posed_serial;
    base = m.ring_base;
    return true;
  }
  if (m.seen_epoch != ep) {   // the first draw of this frame: the at-rest count
    m.seen_epoch = ep;
    if (m.seen_serial == m.posed_serial) ++m.still;
    else m.seen_serial = m.posed_serial, m.still = 0;
  }
  uint32_t off = 0;
  constexpr uint32_t kRestFrames = 4;   // more than the frames in flight (2): a ring slot a frame in flight still reads is never the model's only copy
  if (m.still <= kRestFrames && PosedPush(fb, m.posed_cpu.data(), uint32_t(m.posed_cpu.size()), off)) {
    m.ring_epoch = ep, m.ring_serial = m.posed_serial, m.ring_base = off;
    base = off;
    return true;
  }
  // at rest, or no room: the model's own buffer takes the block (once per update), this and every later draw of this update reads it
  renderer_.UpdateBuffer(m.vb, 0, m.posed_cpu.data(), m.posed_cpu.size(), rdr::kBufPosed);
  m.vb_serial = m.posed_serial;
  return false;
}

// S103 R: replace a model's CPU-side state (light sets, placements, each mesh's placement / light-set choice) from `state`, a skeleton whose meshes are exactly the
// model's drawn meshes in order (no vertices needed). The arena: the record decides which slots show and which light set each uses (arena_apply.h). No GPU work.
bool FrontEnd::UpdateModelState(int id, const assets::Model& state) {
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return false;
  ModelData& m = Mod(id);
  if (state.meshes.size() != m.meshes.size()) return false;
  m.light_sets = state.light_sets;
  m.placements.clear();
  for (const assets::Placement& p : state.placements) m.placements.push_back(p.transforms);
  for (size_t i = 0; i < m.meshes.size(); ++i) {
    const assets::Mesh& me = state.meshes[i];
    m.meshes[i].placement = (me.placement >= 0 && size_t(me.placement) < m.placements.size()) ? me.placement : -1;
    m.meshes[i].light_set = (me.light_set >= 0 && size_t(me.light_set) < m.light_sets.size()) ? me.light_set : -1;
  }
  return true;
}

bool FrontEnd::SetStageCull(int id, const std::vector<uint8_t>& by_slot) {   // S103 V1
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return false;
  ModelData& m = Mod(id);
  if (m.stage_cull == by_slot) return false;
  m.stage_cull = by_slot;
  return true;
}

bool FrontEnd::SetIdleScreens(int id, uint16_t mask) {   // S103 SCF
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return false;
  ModelData& m = Mod(id);
  if (m.idle_screens == mask) return false;
  m.idle_screens = mask;
  return true;
}

void FrontEnd::ReleaseModel(int id) {
  if (id < 0 || size_t(id) >= models_.size() || !models_[size_t(id)]->live) return;
  ModelData& m = Mod(id);
  m.live = false;
  if (draw_lists_) draw_lists_->ReleaseAsset(uint32_t(id));  // S103 G: only this model's kept draws go
  LeavePage(m, false);   // S103 UP: the shared page is the group's: one reference back, destroyed with the last
  if (!m.scr_pool.empty()) {   // S103 UPL U2: the screens' pool owns its images (a shared one is in several slots of `images`): each is destroyed once, here
    for (size_t t = 0; t < m.scr_of.size() && t < m.images.size(); ++t)
      if (m.scr_of[t] >= 0) m.images[t] = rdr::kNoImage;   // no-rebind: the model is dead (live = false), nothing draws it again
    for (const ModelData::ScrImg& e : m.scr_pool)
      if (e.image != rdr::kNoImage) renderer_.DestroyImage(e.image);
    scr_images_ -= std::min<uint64_t>(scr_images_, m.scr_pool.size());
    m.scr_pool.clear(), m.scr_of.clear(), m.scr_moved_at.clear();
  }
  if (!m.images_shared && !m.images_external)  // S103 R: a sharing model leaves the owner's images; S103 D: images from a shared map belong to its owner (LiveCrowd)
    for (rdr::ImageId image : m.images)
      if (image != rdr::kNoImage) renderer_.DestroyImage(image);
  m.images.clear();
  if (m.vb != rdr::kNoBuffer) renderer_.DestroyBuffer(m.vb);
  for (rdr::BufferId b : m.pcopy) renderer_.DestroyBuffer(b);   // S103 PF2: the mapped copies (the renderer frees them kDestroyDelayFrames later)
  m.pcopy.clear(), m.pcopy_ptr.clear(), m.pcopy_use.clear(), m.pcopy_cur = -1;
  if (m.ib != rdr::kNoBuffer) renderer_.DestroyBuffer(m.ib);
  if (m.svb != rdr::kNoBuffer) renderer_.DestroyBuffer(m.svb);
  m.vb = m.ib = m.svb = rdr::kNoBuffer;
  for (auto& kv : m.cloth_bufs)
    if (kv.second != rdr::kNoBuffer) renderer_.DestroyBuffer(kv.second);
  m.cloth_bufs.clear();
}

// ---- textures -----------------------------------------------------------------------------------------------------------------

static size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) { return upload::UploadMipBytes(f, w, h); }   // S108 AL1: the one formula (upload_check.h)

rdr::ImageId FrontEnd::MakeTexture(const assets::Texture& t, std::string& warning) {
  if (t.faces == 6 && !g_reflections.load(std::memory_order_relaxed)) return rdr::kNoImage;   // S103 RFL: switch off = nothing uploaded, no warning (the materials bind the black stand-in)
  upload::TexturePlan plan;   // S108 AL1: the refusals are pure (upload_check.h TextureRefusal: the unit runs them); AddModelImpl reports a refusal through the shared draw_shortfall
  if (const char* why = upload::TextureRefusal(t, plan)) { warning = why; return rdr::kNoImage; }
  const uint32_t fit = plan.fit;
  const size_t total = plan.total;
  const bool cube = plan.cube;
  rdr::ImageDesc d;
  d.width = uint16_t(t.width);
  d.height = uint16_t(t.height);
  d.mips = uint8_t(fit);
  d.usage = rdr::kUsageSampled;
  if (cube) d.layers = 6, d.kind = rdr::ImageKind::Cube;
  switch (t.format) {
    case assets::TexFormat::kBC1: d.format = rdr::Format::BC1; break;
    case assets::TexFormat::kBC2: d.format = rdr::Format::BC2; break;
    case assets::TexFormat::kBC3: d.format = rdr::Format::BC3; break;
    case assets::TexFormat::kB8G8R8A8: d.format = rdr::Format::BGRA8; break;
    default: d.format = rdr::Format::RGBA8; break;  // R8G8B8A8, and B4G4R4A4 widened below
  }
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id == rdr::kNoImage) { warning = "CreateImage failed"; return rdr::kNoImage; }
  std::vector<uint8_t> wide;
  for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face) {
    size_t src = size_t(face) * total;
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
        renderer_.UpdateImage(id, i, face, wide.data(), wide.size());
      } else {
        renderer_.UpdateImage(id, i, face, t.data.data() + src, sb);
      }
      src += sb;
    }
  }
  return id;
}

// ---- models -------------------------------------------------------------------------------------------------------------------

uint32_t FrontEnd::ModelNodeCount(int model) const {
  return model >= 0 && size_t(model) < models_.size() && models_[size_t(model)] ? models_[size_t(model)]->node_count : 0u;  // S103 B
}

uint32_t FrontEnd::ModelMainNodeCount(int model) const {   // S103 W2B: the body's own node count; the full count when the model has no piece nodes
  const uint32_t n = ModelNodeCount(model);
  return model >= 0 && size_t(model) < models_.size() && models_[size_t(model)] && models_[size_t(model)]->main_node_count ? models_[size_t(model)]->main_node_count : n;
}

const std::vector<std::pair<uint32_t, uint32_t>>& FrontEnd::ModelExtraTags(int model) const {   // S103 W2B: (piece id, piece node) of the bones after the body's
  static const std::vector<std::pair<uint32_t, uint32_t>> kNone;
  return model >= 0 && size_t(model) < models_.size() && models_[size_t(model)] ? models_[size_t(model)]->extra_tags : kNone;
}

const std::vector<uint32_t>& FrontEnd::ModelExtraPieceNodes(int model) const {   // S103 W2B (review G)
  static const std::vector<uint32_t> kNone;
  return model >= 0 && size_t(model) < models_.size() && models_[size_t(model)] ? models_[size_t(model)]->extra_piece_nodes : kNone;
}

const FrontEnd::ModelExtraInfo* FrontEnd::ModelExtraInfoOf(int model, size_t e) const {   // S103 W2B
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]) return nullptr;
  const auto& v = models_[size_t(model)]->extra_info;
  return e < v.size() ? &v[e] : nullptr;
}

bool FrontEnd::ModelBounds(int model, float bmin[3], float bmax[3]) const {   // S103 CAWB (census only)
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]) return false;
  for (int k = 0; k < 3; ++k) bmin[k] = models_[size_t(model)]->bmin[k], bmax[k] = models_[size_t(model)]->bmax[k];
  return true;
}

bool FrontEnd::ModelHasPieceMesh(int model, uint32_t piece_id) const {   // S103 PZF2 (EB): a drawable mesh of a wrestler's piece entry (src_kind 2, src_key == the id)
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]) return false;
  for (const ModelData::Mesh& m : models_[size_t(model)]->meshes)
    if (m.src_kind == 2 && m.src_key >= 0 && uint32_t(m.src_key) == piece_id) return true;
  return false;
}

int FrontEnd::AddModel(const assets::Model& model, std::vector<std::string>& warnings, std::unordered_map<std::string, rdr::ImageId>* shared) {
  return AddModelImpl(model, warnings, -1, shared);
}

// S103 R: a model that uses the images of model `image_owner` (the crowd's posed bodies: one body, many poses, one set of textures).
int FrontEnd::AddModelSharing(const assets::Model& model, int image_owner, std::vector<std::string>& warnings) { return AddModelImpl(model, warnings, image_owner, nullptr); }

void FrontEnd::SetUnlit(int model, bool unlit) {  // S103 L4
  if (model >= 0 && size_t(model) < models_.size()) models_[size_t(model)]->unlit = unlit;
}

// S103 GSTP: the GPU group of a model's draws, named by its OWNER (rf_c_core Register by Part, the posed ring / weapon pools); AddModelImpl's label rule (model_group.h) is the fallback.
void FrontEnd::SetModelGroup(int model, uint8_t group) {
  if (model >= 0 && size_t(model) < models_.size() && group < rdr::kGrpCount) models_[size_t(model)]->group = group;
}

void FrontEnd::SetHookOwned(int model, bool owned) {  // S103 D
  if (model >= 0 && size_t(model) < models_.size()) models_[size_t(model)]->hook_owned = owned;
}

std::string FrontEnd::ImageKey(const assets::Texture& t) {
  return t.name + "|" + std::to_string(t.width) + "x" + std::to_string(t.height) + "|" + std::to_string(int(t.format)) + "|" + std::to_string(t.data.size());
}
void FrontEnd::ReleaseImage(rdr::ImageId id) {
  if (id != rdr::kNoImage) renderer_.DestroyImage(id);   // deferred by the renderer (kDestroyDelayFrames): nothing waits
}

int FrontEnd::AddModelImpl(const assets::Model& model, std::vector<std::string>& warnings, int image_owner, std::unordered_map<std::string, rdr::ImageId>* shared) {
  if (image_owner >= 0 && size_t(image_owner) < models_.size() && models_[size_t(image_owner)]->live && models_[size_t(image_owner)]->page_group != 0) {
    // S103 UP lint: a sharer copies the owner's image ids WITHOUT a group reference; the owner's page image is the group's and dies with the group. Refused, counted (the unit / rig read it).
    ++page_share_violations_;
    rdr::Log("[rdr] UPS ASSERT: AddModelSharing from model %d, which sits on page group %llu (refused)", image_owner, (unsigned long long)models_[size_t(image_owner)]->page_group);
    return -1;
  }
  auto* m = new ModelData();
  m->label = model.label;
  m->group = GroupOfLabel(m->label);   // S103 GSTP
  m->images_external = shared != nullptr;  // S103 D
  const bool share = image_owner >= 0 && size_t(image_owner) < models_.size() && models_[size_t(image_owner)]->live;  // S103 R
  if (share) {
    m->images = models_[size_t(image_owner)]->images;
    m->img_info = models_[size_t(image_owner)]->img_info;
    m->images_shared = true;
  }
  for (const assets::Texture& t : model.textures) {
    if (share) break;
    std::string w;
    const std::string skey = ImageKey(t);
    rdr::ImageId id = rdr::kNoImage;
    if (shared) { const auto f = shared->find(skey); if (f != shared->end()) id = f->second; }
    if (page_group_build_ != 0 && !shared && !g_plant_page_per_model && int(m->images.size()) == model.stage_lightmap && m->page_group == 0) {   // S103 UP: the page texture of a model of a page group IS the group's image: no private copy made, none uploaded
      const auto g = page_shares_.find(page_group_build_);
      if (g != page_shares_.end() && t.format == assets::TexFormat::kR8G8B8A8 && std::max(1u, t.mips) == 1 && t.width == g->second.w && t.height == g->second.h) {
        id = g->second.image, ++g->second.refs;
        m->page_tex = int(m->images.size()), m->page_group = page_group_build_;
      }
    }
    if (id == rdr::kNoImage) {
      assets::Texture leveled;   // S111 MIPLEVELS: the levels the game's loader gives this texture (game_levels.h Relevel: cut to its count, or the missing ones box-filtered); `t` stays the file's
      id = MakeTexture(game_levels::Relevel(t, leveled) ? leveled : t, w);
      if (shared && id != rdr::kNoImage) (*shared)[skey] = id;
    }
    if (id == rdr::kNoImage && !w.empty()) warnings.push_back("texture '" + t.name + "': " + w), upload::ReportUploadRefusal("texture_upload", model.label, "texture '" + t.name + "'", w);   // (S103 RFL: a cube skipped with the switch off says nothing); S108 AL1: the shared draw_shortfall
    if (static const bool dump_tex = std::getenv("WWE_RDR_DUMP_MATS") != nullptr; dump_tex) std::fprintf(stderr, "[texs] %-16s format=%d %ux%u mips=%u\n", t.name.c_str(), int(t.format), t.width, t.height, t.mips);  // lane 257 (dev)
    m->images.push_back(id);
    m->img_info.push_back({t.width, t.height, std::max(1u, t.mips), t.format == assets::TexFormat::kR8G8B8A8, int(t.format), id != rdr::kNoImage ? std::max(1u, t.faces) : 1u});
    if (t.faces == 6 && id != rdr::kNoImage) {   // S103 RFL census: the cube files this model loaded (name WxH, the first few distinct)
      ++reflect_cubes_;
      char one[64];
      std::snprintf(one, sizeof one, "%s %ux%u", t.name.c_str(), t.width, t.height);
      if (reflect_cube_names_.size() < 160 && reflect_cube_names_.find(one) == std::string::npos) reflect_cube_names_ += (reflect_cube_names_.empty() ? "" : ", ") + std::string(one);
    }
  }
  m->materials = model.materials;
  {  // S103 TQF: a prop slot's cut-out (the loader kept its texel verdict in texel_cutout bit 1, no longer applied) stays a sorted, game-order cut-out where the effect table's variant shows the diffuse alpha, or when there is no table
    const std::shared_ptr<const read_rule::EffectTable> tbl = read_rule::EffectTableNow();
    for (assets::Material& mt : m->materials) {
      if (!(mt.texel_cutout & 2) || mt.alpha_blend) continue;
      bool shows = true;   // no table / no row: the old texel behaviour
      uint64_t ph = 0;
      if (tbl && technique_pick::PsHash(*tbl, mt.shader, mt.technique, ph)) {
        const int v = FragmentOfHash(ph, mt.shader);
        if (v >= 0 && T().by_index[size_t(v)]) shows = technique_pick::ShowsDiffuseAlpha(T().by_index[size_t(v)]->defines);
      }
      if (shows) mt.alpha_blend = true, mt.cutout_blend = 1;
    }
  }
  m->slot_records = model.slot_records;   // S103 D1
  m->arena_id = model.arena;   // S103 D1
  for (const assets::Placement& p : model.placements) m->placements.push_back(p.transforms);
  m->light_sets = model.light_sets;
  m->node_count = uint32_t(model.bones.size());  // S103 B
  if (model.main_bones && model.main_bones <= model.bones.size()) {   // S103 W2B: the piece-added bones and their tags (rf_wrestlers compares them with the record's)
    m->main_node_count = model.main_bones;
    for (size_t bi = model.main_bones; bi < model.bones.size(); ++bi) {
      const assets::Bone& eb = model.bones[bi];
      m->extra_tags.emplace_back(uint32_t(eb.piece_id), uint32_t(eb.piece_node));   // (-1 = a bone no piece added: never equals a record tag)
      FrontEnd::ModelExtraInfo xi;
      xi.parent = eb.parent;
      const assets::Matrix fw = assets::MirrorY(eb.bind_world);   // bind_world is viewer space; the game's rows are in file space
      xi.pos[0] = fw[12], xi.pos[1] = fw[13], xi.pos[2] = fw[14];
      xi.has_corr = eb.has_corr;
      if (eb.has_corr) std::memcpy(xi.corr_inv, eb.corr_inv.data(), sizeof xi.corr_inv);
      m->extra_info.push_back(xi);
      m->extra_piece_nodes.push_back(eb.piece_nodes);
    }
  }
  for (int k = 0; k < 3; ++k) m->bmin[k] = model.bounds_min[k], m->bmax[k] = model.bounds_max[k];   // S103 CAWB
  m->stage_lm = (model.stage_lightmap >= 0 && size_t(model.stage_lightmap) < m->images.size() && m->images[size_t(model.stage_lightmap)] != rdr::kNoImage)
                    ? model.stage_lightmap : -1;
  m->lm_xy[0] = model.lm_scale[0], m->lm_xy[1] = model.lm_scale[1], m->lm_base_gain = model.lm_gain, m->lm_card = model.lm_card;
  ApplyLmRow(*m);
  std::vector<uint8_t> vb, ib, svb;
  int src_index = -1;
  for (const assets::Mesh& me : model.meshes) {
    if (me.vertices.empty() || me.indices.size() < 3) continue;
    bool bad = false;
    for (uint32_t ix : me.indices) bad = bad || ix >= me.vertices.size();
    if (bad) { warnings.push_back("mesh '" + me.part + "': index out of range, skipped"), upload::ReportUploadRefusal("mesh_upload", model.label, "mesh '" + me.part + "'", "index out of range, skipped"); continue; }   // S108 AL1
    if (me.part.rfind("crowd flash", 0) == 0) continue;  // the flash sprite is the viewer's flash pass, not a scene mesh
    ++src_index;
    const int material = (me.material >= 0 && size_t(me.material) < m->materials.size()) ? me.material : -1;
    std::vector<uint32_t> src_extra;   // S103 W2B (review F2): the piece ids whose extra (spring-hair) nodes ANY chunk of this source mesh skins to (SceneFrame::absent_extras skips all its chunks)
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
      {   // S103 E5
        int sn = -1;
        if (std::sscanf(me.part.c_str(), "slot %d", &sn) == 1 && sn >= 0) mm.slot_no = sn;
        mm.src_record = me.src_record;
        mm.src_kind = me.src_kind, mm.src_key = me.src_key, mm.node = me.node, mm.src_pac = me.src_pac;
        mm.answers_piece_row = MeshAnswersPieceRow(me.src_kind);   // S103 PZF2; S111 GUESTKEY: a guest mesh carries the stock key (guest_model_draw.h BuildModel), so no pac test
        mm.piece_cut = piece_draw::PieceCut(mm.answers_piece_row, me.src_key, material >= 0 && size_t(material) < m->materials.size() && m->materials[size_t(material)].alpha_blend);   // S108 HAIR
        mm.lid_layer = me.src_kind == 2 && me.src_key >= 20000 && me.src_key < 40000 && material >= 0 && size_t(material) < m->materials.size() &&
                       m->materials[size_t(material)].shader.find("trans") != std::string::npos && me.palette.find("mabuta") != std::string::npos;   // S103 EYF
        mm.uv_scroll = material >= 0 && size_t(material) < m->materials.size() && m->materials[size_t(material)].shader.find("UVScrol") != std::string::npos;   // S103 RCL: "UVScrol" (not "UVScroll"): the game names one of the four Time-reading shaders yProp_UVScrolLM (src/renderer/shaders/fx/*.fx.json with a "Time" field: yBG_UVScroll, yBG_UVScrollLM, yProp_UVScroll, yProp_UVScrolLM)
      }
      if (chunk) {
        mm.chunk = true;
        m->skinned_model = true;   // S111 OLDCHAIN review: the pass rule's model-skinned bit (CharPassOf)
        mm.nodes = chunk->nodes;
        mm.extra_pieces = src_extra;   // S103 W2B (review F2): the SOURCE mesh's pieces, on every chunk of it: a partial hair mesh never draws
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
    if (me.skinned && material >= 0 && (IsCharEffect(m->materials[size_t(material)].shader) || HasSkinnedProgram(m->materials[size_t(material)].shader))) {
      // S101 lane 13: a character piece is cut into chunks of at most 21 bones (the effects hold 21 bone matrices), each with its own bone stream.
      std::vector<SkinChunk> chunks;
      if (!BuildSkinChunks(me, chunks)) { warnings.push_back("mesh '" + me.part + "': a triangle needs more than 21 bones, skipped"), upload::ReportUploadRefusal("mesh_upload", model.label, "mesh '" + me.part + "'", "a triangle needs more than 21 bones, skipped"); continue; }   // S108 AL1
      if (m->main_node_count)
        for (const SkinChunk& c : chunks)
          for (uint16_t nd : c.nodes)
            if (nd >= m->main_node_count && size_t(nd - m->main_node_count) < m->extra_tags.size()) {
              const uint32_t pid = m->extra_tags[size_t(nd - m->main_node_count)].first;
              if (std::find(src_extra.begin(), src_extra.end(), pid) == src_extra.end()) src_extra.push_back(pid);
            }
      for (const SkinChunk& c : chunks) add_piece(c.vertices, c.indices, &c);
    } else {
      add_piece(me.vertices, me.indices, nullptr);
    }
  }
  if (!vb.empty()) m->vb = renderer_.CreateBuffer(vb.size(), vb.data());
  m->vb_bytes = uint32_t(vb.size());   // S103 PVR
  if (!ib.empty()) m->ib = renderer_.CreateBuffer(ib.size(), ib.data());
  if (!svb.empty()) m->svb = renderer_.CreateBuffer(svb.size(), svb.data());
  m->bound.resize((m->materials.size() + 1) * 128);   // S108 THB: x2 the entrance prop's change-colour bind;   // S103 RML: x2 the ring mat's props-map (light) variant;   // S103 SWF: x2 the liquid-map (wet) variant; S103 L6: x2 the shadow-reader variant; S103 L10: x4 the light-map mode (was a bool x2)
  //  // x2 rigid / skinned, x2 entrance light map (S101 lane 15)  // x2: rigid / skinned
  models_.push_back(m);
  return int(models_.size()) - 1;
}

// ---- binding a material to an effect -----------------------------------------------------------------------------------------

// S105 SZW: does the pixel variant have an alpha twin that ProgramFor would use (the same test as its `twin`)? The discard of the game's stage alpha test needs one.
static bool VariantHasTwin(int ps) {
  if (ps < 0 || !kVariants[ps].bytes_alpha) return false;
  const ShaderReload* reload = ShaderReload::Get();
  return !(reload && reload->DropsTwin(ps));
}

uint16_t FrontEnd::ProgramFor(int vs, int ps, bool alpha) {
  const uint64_t key = uint64_t(uint32_t(vs)) << 32 | uint64_t(uint32_t(ps + 1)) << 1 | uint64_t(alpha);
  const auto found = programs_.find(key);
  if (found != programs_.end()) return found->second;
  const rdr::fx::Variant& vertex = kVariants[vs];
  rdr::ProgramDesc desc;
  names_.push_back(std::string(vertex.name) + " + " + kVariants[ps].name);
  desc.name = names_.back().c_str();
  desc.vertex_spirv = rdr::fx::Words(size_t(vs), false);
  desc.vertex_bytes = vertex.bytes;
  const rdr::fx::Variant& fragment = kVariants[ps];
  const ShaderReload* reload = ShaderReload::Get();  // S101 lane24
  const bool twin = alpha && fragment.bytes_alpha != 0 && !(reload && reload->DropsTwin(ps));  // (a reload that dropped the twin: "alpha_test": false)
  desc.fragment_spirv = rdr::fx::Words(size_t(ps), twin);
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
    desc.vertex_spirv = rdr::fx::Words(size_t(vs), false), desc.vertex_bytes = vertex.bytes;
    desc.fragment_spirv = rdr::fx::Words(size_t(ps), twin), desc.fragment_bytes = twin ? fragment.bytes_alpha : fragment.bytes;
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

// S103 lane D: the instanced twin of a plain yAudience variant (yAudience_<x>_vertex -> yAudienceInst_<x>_vertex), -1 when there is none or its constant block differs from the plain one's
// (the block is pushed as the plain variant packs it).
static int InstTwinOf(int vs) {
  static std::unordered_map<int, int> cache;
  const auto found = cache.find(vs);
  if (found != cache.end()) return found->second;
  int result = -1;
  const char* name = vs >= 0 ? kVariants[vs].name : nullptr;
  static const char kPrefix[] = "yAudience_";
  if (name && std::strncmp(name, kPrefix, sizeof kPrefix - 1) == 0) {
    const std::string twin = std::string("yAudienceInst_") + (name + sizeof kPrefix - 1);
    for (int i = 0; i < kVariantCount; ++i)
      if (kVariants[i].name && twin == kVariants[i].name) {
        const rdr::fx::Variant& a = kVariants[vs];
        const rdr::fx::Variant& b = kVariants[i];
        bool same = a.block_bytes == b.block_bytes && a.constant_count == b.constant_count && a.bools_offset == b.bools_offset;
        for (uint16_t k = 0; same && k < a.constant_count; ++k)
          same = a.constants[k].reg == b.constants[k].reg && a.constants[k].count == b.constants[k].count && a.constants[k].offset == b.constants[k].offset;
        if (same) result = i;
        break;
      }
  }
  cache.emplace(vs, result);
  return result;
}

uint16_t FrontEnd::InstLayoutFor(int vs) {   // S103 lane D (CrowdOurs::InstLayoutFor's layout: attributes 0-2 over InstVertex, binding 1 = the world rows at 4-6)
  const auto found = inst_layouts_.find(vs);
  if (found != inst_layouts_.end()) return found->second;
  constexpr uint16_t kRGB32F = 106, kRG32F = 103, kRGBA32F = 109;
  uint16_t id = 0xFFFF;
  const fxt::VariantTable* t = T().by_index[size_t(vs)];
  bool supported = t != nullptr && t->input_count > 0;
  rdr::VertexLayoutDesc desc{};
  desc.strides[0] = uint32_t(sizeof(InstVertex));
  desc.strides[1] = 48;
  desc.per_instance[1] = 1;
  for (uint32_t k = 0; supported && k < t->input_count && desc.attribute_count + 3 < rdr::kMaxVertexAttributes; ++k) {
    const std::string name = t->inputs[k].name;
    rdr::VertexAttribute a{};
    a.location = t->inputs[k].location;
    if (name == "inPosition" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(InstVertex, pos));
    else if (name == "inNormal" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(InstVertex, normal));
    else if (name == "inUv" && t->inputs[k].components == 2) a.format = kRG32F, a.offset = uint32_t(offsetof(InstVertex, uv));
    else supported = false;
    desc.attributes[desc.attribute_count++] = a;
  }
  if (supported && desc.attribute_count != t->input_count) supported = false;
  if (supported) {
    desc.attributes[desc.attribute_count++] = {4, 1, kRGBA32F, 0};
    desc.attributes[desc.attribute_count++] = {5, 1, kRGBA32F, 16};
    desc.attributes[desc.attribute_count++] = {6, 1, kRGBA32F, 32};
    id = renderer_.AddVertexLayout(desc);
  }
  inst_layouts_.emplace(vs, id);
  return id;
}

static uint64_t CharBoundKey(int material, bool skinned, int pass, bool change_colour = false) { return (uint64_t(uint32_t(material + 1)) << 8) | (change_colour ? 0x40u : 0u) | (skinned ? 0x80u : 0u) | uint64_t(pass & 0x1F); }   // S103 YCB   // S108 THR2: + the change-colour bit (0x40: pass < 32)

const FrontEnd::Bound& FrontEnd::BindMaterial(ModelData& m, int material, bool skinned, LmMode lm_mode, bool shadow, bool wet, int char_pass, bool light, bool change_colour) {
  // S108 THR2: a wrestler body's change-colour bind exists only with the table pick (char_pass >= 0). S108 THB: an entrance PROP's (char_pass < 0, not a character effect) is its own bind slot (the cc bit below).
  const bool prop_cc = change_colour && char_pass < 0 && material >= 0 && !IsCharEffect(m.materials[size_t(material)].shader) && !shadow && !wet && !light;
  if (char_pass < 0) change_colour = false;
  if (char_pass >= 0) {   // S103 YCB: the pass-keyed cache of the wrestler table pick (a bind made before the effect table existed is rebuilt once it does)
    const auto it = m.char_bound.find(CharBoundKey(material, skinned, char_pass, change_colour));
    if (it != m.char_bound.end() && it->second && !(it->second->no_table && read_rule::EffectTableNow())) return *it->second;
  }
  const size_t slot = ((((((material >= 0 ? size_t(material) : m.materials.size()) * 2 + (skinned ? 1 : 0)) * 4 + size_t(lm_mode)) * 2 + (shadow ? 1 : 0)) * 2 + (wet ? 1 : 0)) * 2 + (light ? 1 : 0)) * 2 + (prop_cc ? 1 : 0);   // S108 THB: x2 the entrance prop's change-colour bind   // S103 SWF: x2 wet   // S103 L10: x4 light-map mode (was x2 entrance); S103 L6: x2 shadow  // S101 lane 15: x2 entrance light map
  if (char_pass < 0 && m.bound[slot] && !(m.bound[slot]->no_table && read_rule::EffectTableNow())) return *m.bound[slot];   // S103 TQF: a technique-class bind made before the effect table existed is rebuilt
  auto b = std::make_unique<Bound>();
  assets::Material fallback;
  fallback.shader = "(no material)";
  const assets::Material& mat = material >= 0 ? m.materials[size_t(material)] : fallback;
  const auto tex = [&](int i) -> rdr::ImageId {   // S103 RFL: a cube image never reaches a 2D slot
    return (i >= 0 && size_t(i) < m.images.size() && !(size_t(i) < m.img_info.size() && m.img_info[size_t(i)].faces == 6)) ? m.images[size_t(i)] : rdr::kNoImage;
  };
  if (IsCharEffect(mat.shader)) {  // S101 lane 13: the wrestlers' effects
    b->chr = true;
    b->hair = IsHairEffect(mat.shader);
    const auto cube_of = [&](int i) { return env_cube::CubeAt(m.images, m.img_info, i, rdr::kNoImage); };   // S111 ENVCUBE: a six-face image of the model (slot 10's cube), else none
    // S103 YCB (YCH.md) / S111 OLDCHAIN: the pair is the game's table row for (technique, pass) (ChooseCharEffectTable), the ONLY pick of a wrestler material; shadow / wet are read from the picked pair.
    // A row that cannot be bound as is (no effect table, no row, a blocked row) is a HOLE: no pair (the unbound branch below: debug, Record draws nothing), counted here per distinct bind by its reason
    // (CharHoleOf: the census line's char_hole_no_table / char_hole_no_row / char_wrinkle_blocked) and per draw by Record (char_hole_draws); each one a bug lane. A wrestler material bound without a pass gets no pair either.
    CharChoice cc;
    if (char_pass >= 0) {
      cc = ChooseCharEffectTable(mat.shader, skinned, mat.alpha_test, mat.technique, char_pass, mat.logo7 >= 0 || mat.logo4 >= 0, change_colour);   // S105 A5c fix F1: a logo material takes the editor table's row   // S108 THR2: a recoloured body the STS row
      b->change_colour = cc.Ok() && cc.change_colour_table;
      if (cc.no_table) b->no_table = true;   // rebuilt once the table exists (BindMaterial's char cache check); one table snapshot, taken inside the pick (review F4)
      const CharHole hole = CharHoleOf(cc);
      switch (hole) {
        case CharHole::None:
          ++char_table_picked_, b->char_table = true, b->cube_analytic = cc.cube_analytic;
          if (cc.cube_analytic) ++char_cube_analytic_;
          break;
        case CharHole::NoTable: ++char_hole_no_table_; break;
        case CharHole::NoRow: ++char_hole_no_row_; break;
        case CharHole::Wrinkles: ++char_wrinkle_blocked_; break;
      }
      if (hole != CharHole::None) {   // S111 OLDCHAIN review: ONE line per distinct hole (the row and its reason), naming the first material that hit it: each line is a bug lane (no line cap: the distinct rows are few)
        static std::set<std::string> hole_said;
        if (hole_said.insert(mat.shader + '|' + std::to_string(mat.technique) + '|' + std::to_string(char_pass) + (skinned ? "|s|" : "|r|") + (mat.alpha_test ? "a|" : "|") + cc.why_not).second)
          rdr::Log("[scene][front_end] char table HOLE (%s): %s technique %u pass %d %s%s (material %d, diffuse '%s'): %s", CharHoleCounter(hole), mat.shader.c_str(), unsigned(mat.technique), char_pass,
                   skinned ? "skinned" : "rigid", mat.alpha_test ? " alpha-tested" : "", material, mat.diffuse_name.c_str(), cc.why_not.c_str());
      }
      shadow = cc.shadow, wet = cc.wet;
    } else {
      cc.why_not = "a wrestler material is bound only by its pass (the effect table's row)";
    }
    b->shadow = shadow && cc.Ok();
    b->wet = wet && cc.Ok();
    b->choice.vs = cc.vs, b->choice.ps = cc.ps, b->choice.effect = mat.shader, b->choice.why_not = cc.why_not;
    EffectCoverage& cov = coverage_[mat.shader];
    cov.effect = mat.shader;
    if (cc.Ok()) {
      const std::string pair = std::string(kVariants[cc.vs].name) + " + " + kVariants[cc.ps].name;
      if (cov.bound_to.empty() || (shadow && cov.bound_to.find("_shadow") == std::string::npos)) cov.bound_to = pair;   // S103 L6: the shadow pair is reported once any frame binds it
      // ALPHA-SOURCE (S111 ALPHAWREST): the game alpha-tests every COLOUR-pass draw of a character node, never by a material flag (its shadow-map / rim passes, model slots 47 sub_82C01FE0 / 49 sub_82BF9878
      // through sub_82C13C88, turn the test and the blend off; this bind serves the colour pass only). The opaque node sub_825A3908 and the model draw sub_82C04DF0 set no alpha test, so the
      // body draws with the category reset's (sub_82C22E88: test on, GEQUAL, ref 1, blend on); the translucent node sub_8259FAE8 turns it on at GEQUAL ref 1.
      // S111 CHALPHA (char_alpha.h; 360_HABITS.md row 32: a HABIT where the alpha is exactly 1): the test + blend are kept only where the pixel's alpha can fall below 1, by the bound pixel variant's
      // registry class (Variant::out_alpha: compile_shaders.py CH_TINT_ALPHA, from the game's microcode). A pair whose alpha may carry a texture (yCh_trans / TransTiling / cloth_trans / Aniso_trans / Ls)
      // is tested here on every draw; a tint-only pair (alpha = MdlAlpha x MatAmbCol.w x MatDifCol.w, out_alpha 1) is tested by Record on a draw whose constants are not all 1 (tint_discard).
      // The reference is the pass class's (pass_state.h kTable; a piece cut's 128 through Record); a tested opaque draw blends (tested_blends).
      b->alpha_test = char_alpha::BindTests(kVariants[cc.ps].out_alpha, VariantHasTwin(cc.ps));
      b->program = ProgramFor(cc.vs, cc.ps, b->alpha_test);
      b->layout = LayoutFor(cc.vs);
      if (const fxt::VariantTable* vt = T().by_index[size_t(cc.vs)])   // S103 WRK (review F4): the vertex variant declares the level fields (WklLvLst / IdMapLvLst): only then are levels joined and folded into the stamp
        for (uint32_t k = 0; k < vt->field_count; ++k) {
          const std::string fnm = vt->fields[k].name;
          if (fnm == "WklLvLst" || fnm == "IdMapLvLst") b->wrk_fields = true;
        }
      b->reads_wrinkles = std::strstr(kVariants[cc.ps].name, "_wrinkles") != nullptr, b->reads_idmap = std::strstr(kVariants[cc.vs].name, "id_map") != nullptr;   // S103 XLF
      const rdr::fx::Variant& fv = kVariants[cc.ps];
      const bool reads_muscle = T().by_index[size_t(cc.ps)] && muscle_map::ReadsMuscle(T().by_index[size_t(cc.ps)]->defines);   // S111 MUSCLEMAP: the fragment's FX_MUSCLE
      for (uint32_t s = 0; s < fv.sampler_count; ++s) {
        const int slotn = fv.samplers[s].slot;
        rdr::ImageId image = white_;
        switch (muscle_map::Role(slotn, b->change_colour, wet, reads_muscle)) {   // S108 THR2: a change-colour bind's 7 / 4 / 14 / 2 are the masks; S111 MUSCLEMAP: an FX_MUSCLE fragment's 2 is the muscle map
          case CharSlot::ChangeMask1: case CharSlot::ChangeMask2: case CharSlot::ChangeMask3: case CharSlot::ChangeMask4: {   // S108 THR2: Mask1..4 (sub_8339F0E0), the model's own (change_mask.h); none = black (that mask does nothing)
            const int mi = change_colour_draw::MaskTextureOfSlot(mat, slotn);
            image = tex(mi) != rdr::kNoImage ? tex(mi) : black_;
            break;
          }
          case CharSlot::LiquidMap: image = black_, b->liquid_slot = uint8_t(slotn); break;   // S103 SWF: the wrestler's liquid map, set at the draw (black = an empty map)
          case CharSlot::SweatBump: image = tex(mat.sweat_bump) != rdr::kNoImage ? tex(mat.sweat_bump) : neutral_normal_; break;   // (a missing bump is the neutral normal, never black: black reads as a full-strength tilt, review SWF-10)   // S103 SWF: the material's own bump map
          case CharSlot::MuscleMap: image = muscle_map::Image(mat, tex, rdr::kNoImage, black_); break;   // S111 MUSCLEMAP: the material's g_mMusclePrm map (black = alpha 0: the plain normal map)
          case CharSlot::Diffuse: image = tex(mat.diffuse) != rdr::kNoImage ? tex(mat.diffuse) : white_; break;
          case CharSlot::Normal: image = tex(mat.normal) != rdr::kNoImage ? tex(mat.normal) : (b->hair ? grey_ : neutral_normal_); break;
          case CharSlot::NoiseTile: image = tex(mat.detail) != rdr::kNoImage ? tex(mat.detail) : grey_; break;
          case CharSlot::Mask: image = (wet && liquid_map::Plants().mask_white) ? white_ : tex(mat.mask) != rdr::kNoImage ? tex(mat.mask) : black_; break;   // (mask_white: the rig's dev plant, settles whether slot 6 hides the sweat)
          case CharSlot::LightCube: b->cube_slot[slotn] = 1; break;
          case CharSlot::WrinkleNormal: {   // S103 WRK: `wrinkles_n` on a table-picked bind; a face variant with no map binds the NEUTRAL normal (counted; review F1 / F6); the switch off = the old grey
            const bool have = tex(mat.wrinkle_normal) != rdr::kNoImage;
            const int pick = wrk::WrinkleSlotPick(wrk::Live(), b->char_table, have, g_wrk_plant_grey);
            image = pick == 0 ? tex(mat.wrinkle_normal) : pick == 1 ? neutral_normal_ : grey_;
            if (pick == 1 && std::strstr(kVariants[cc.ps].name, "_wrinkles")) ++wrk_.slot_fallback;
            break;
          }
          case CharSlot::RimMap: image = black_, b->cube_slot[slotn] = 2; break;
          case CharSlot::HairNoise: image = tex(mat.hair_noise_map) != rdr::kNoImage ? tex(mat.hair_noise_map) : grey_; break;   // S111 HAIRMAPS: the material's noise map (slot 8); grey = the old even half-strength second highlight
          case CharSlot::HairShift: image = tex(mat.hair_shift_map) != rdr::kNoImage ? tex(mat.hair_shift_map) : grey_; break;   // S111 HAIRMAPS: the material's shift map (slot 9); grey = no per-strand shift
          case CharSlot::FlatBlack: image = black_; break;
          case CharSlot::Logo7: image = tex(mat.logo7) != rdr::kNoImage ? tex(mat.logo7) : black_; break;   // S105 A5c: the CAW logo (fetch slot 7), black = the layer does nothing
          case CharSlot::Logo4: image = tex(mat.logo4) != rdr::kNoImage ? tex(mat.logo4) : black_; break;   // S105 A5c: logo2 (slot 4)
          case CharSlot::EnvCube: image = cube_of(mat.env_cube) != rdr::kNoImage ? cube_of(mat.env_cube) : black_cube_; b->cube_slot[slotn] = 3; break;   // S111 ENVCUBE: the material's g_mEnvLPrm cube (CUBEMAP_eyes / CUBEMAP, chCommon); black = no environment light
          case CharSlot::ShadowMap: image = white_, b->cube_slot[slotn] = 4; break;   // S103 L6: the frame's shadow map, set at the draw
        }
        if (!options_.textures && image != black_ && !b->cube_slot[slotn]) image = grey_;  // the planted fault: no textures
        b->slot_image[slotn] = image == rdr::kNoImage ? white_ : image;
      }
    } else {
      b->debug = true;
      if (cov.reason.empty() && !shadow) cov.reason = cc.why_not;   // S103 L6: a failed shadow bind falls back to the plain one, its why_not is not the effect's
      if (debug_vs_ >= 0) {
        b->program = ProgramFor(debug_vs_, debug_ps_, false);
        b->layout = LayoutFor(debug_vs_);
      }
    }
    if (char_pass >= 0) {   // S103 YCB: a pass-keyed bind lives in char_bound
      std::unique_ptr<Bound>& cslot = m.char_bound[CharBoundKey(material, skinned, char_pass, change_colour)];
      cslot = std::move(b);
      return *cslot;
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
  const bool ent_lm = lm_mode == LmMode::EntranceLm;   // S101 lane 15: the entrance draws this class with its LM variant
  const bool match_arena = lm_mode == LmMode::MatchVertexPage || lm_mode == LmMode::MatchNoPage;  // S103 L10: a MATCH arena mesh: the game draws it with the plain `yBG_Default` family (no pixel page, stream: run 27 frames 100 / 300 / 500 / 641: slots (0) or (0, 16) only)
  w.lightmap = !match_arena && (own_lm != rdr::kNoImage || mat.has_lm_slot || ent_lm) && lm_image != rdr::kNoImage;
  w.page_available = lm_image != rdr::kNoImage;  // S103 L10
  w.vertex_lm = mat.vertex_lm && !skinned && lm_image != rdr::kNoImage && lm_mode == LmMode::MatchVertexPage;  // (the entrance's program for these meshes is unmeasured: an entrance keeps the per-pixel LM pair only)  // S103 L10: the game's vertex-LM program for this arena mesh (record word +0x90 bit 0): in a MATCH the page in the vertex stage only (mode 2), in an entrance on top of the LM pixel pair (mode 1)
  // S103 L10 (docs/systems/crowd.md "L10"): the game draws every lit crowd body with `yAudience_lit_lm_vertex_vertex` + `yAudience_base_fragment` (frame 500: 80 draws, slots 0 and 16): the page in the VERTEX stage only,
  // no pixel page. Class rule (no per-mesh bit): the audience material, not the far-card quad (m.unlit), not skinned. L4 had the page per pixel (`lit_lm_uv` + `lm_pixel`).
  if (!L10PlantOld() && mat.shader == "yAudience" && !m.unlit && !skinned && lm_image != rdr::kNoImage) w.vertex_lm = true, w.lightmap = false;
  w.skinned = skinned;
  w.normal = tex(mat.normal) != rdr::kNoImage;
  // S101 lane 257: a normal-mapped class (yBG_BumpMap, yProp_BumpMap) whose material has no normal map (arena 40's ent_shd03 card) is drawn with the neutral one (no bump, half gloss:
  // the effect's own no-bump input) instead of the debug colour; the viewer draws such a material plain.
  const bool neutral_normal = !w.normal && mat.shader.find("BumpMap") != std::string::npos && neutral_normal_ != rdr::kNoImage;
  if (neutral_normal) w.normal = true;
  w.specmap = tex(mat.specular) != rdr::kNoImage;
  w.layers = UvLayers(mat, tex, options_.plant_one_layer);   // S103 LAY
  w.planar = w.layers == 2 ? SearchLightPlanar(mat) : 0;   // S103 TQ2 fix F4: a searchlight whose layer texture is missing draws as one layer (the old pick), not as nothing
  w.spec = mat.spec_lev > 0.f && !mat.no_highlight;  // S103 L5: the game's plain arena variants carry no highlight
  w.alpha_test = mat.alpha_test;
  w.unlit = m.unlit;   // S103 L4
  w.alpha_mask = mat.alpha_mask;  // S103 P1
  const bool reflections = g_reflections.load(std::memory_order_relaxed);   // S103 RFL
  const auto cube_of = [&](int i) -> rdr::ImageId {   // S103 RFL: the material's file cube (a 6-face image), kNoImage when it names none / the upload failed / the switch is off
    return (reflections && !options_.plant_cube_unbound && i >= 0 && size_t(i) < m.images.size() && size_t(i) < m.img_info.size() && m.img_info[size_t(i)].faces == 6) ? m.images[size_t(i)] : rdr::kNoImage;
  };
  w.cube = cube_of(mat.cube) != rdr::kNoImage;
  w.refmap = reflections && tex(mat.refmap) != rdr::kNoImage;
  if ((technique_pick::AppliesTo(read_rule::Lower(mat.shader)) || (reflections && technique_pick::ReflectFamily(read_rule::Lower(mat.shader))) || (g_table_pick_all.load(std::memory_order_relaxed) && technique_pick::AllFamily(read_rule::Lower(mat.shader)))) && mat.technique < 4096u) w.technique = int(mat.technique);   // S103 TQF: the game's pixel variant is the effect table's row for the record's technique word (ChooseEffect)  (S103 RFL: + the arena Reflect effects)
  w.change_colour = prop_cc && w.technique >= 0 && mat.change_mask[0] >= 0;   // S108 THB: the entrance prop's recoloured draw: the STS table's cc_mask row, the mask loaded (the caller counts a material without one)
  w.screen_shadow = shadow && !light && IsPlaneReaderEffect(mat.shader);   // S103 L9: the ring mat's plane-shadow variant
  w.light_shadow = shadow && light && IsPlaneReaderEffect(mat.shader);   // S103 RML: ... or the props-map variant (the game's pass 2 / 3 of sub_82608CB0 when it built no plane module)
  b->choice = ChooseEffect(mat.shader, w);
  b->alpha_test = mat.alpha_test || (b->choice.Ok() && GameTestsAlpha(mat, b->choice.ps, b->choice.tech_ps, options_.plant_untested_alpha));   // S103 B2 (TQF: by the variant index; the game's table variant decides, not the stand-in)
  b->change_colour = w.change_colour && b->choice.Ok() && b->choice.cc_row && b->choice.tech_picked;   // S108 THB: bound to the STS cc_mask row (else the plain pick: the draw is stock-coloured and counted)
  if (w.technique >= 0 && !read_rule::EffectTableNow()) b->no_table = true;   // S103 TQF: bound with no effect table: rebuilt once one exists (the cache check at the top)
  if (w.technique >= 0 && !shadow) {   // S103 TQF: counted per colour bind (a yProp_BumpMap / Default / Reflect material)
    if (technique_pick::ReflectFamily(read_rule::Lower(mat.shader))) {   // S103 RFL: the arena Reflect effects count apart from TQF's yProp counters
      if (b->choice.tech_picked) ++bgrefl_picked_;
      else if (b->choice.tech_fallback) ++bgrefl_fallbacks_;
    }
    else if (b->choice.tech_picked) ++tech_picked_;
    else if (b->choice.tech_fallback) { ++tech_fallbacks_; rdr::Log("[scene][front_end] technique fallback: %s technique %u (%d fallbacks, %d table picks)", mat.shader.c_str(), unsigned(mat.technique), tech_fallbacks_, tech_picked_); }
  }
  b->plane = w.screen_shadow && b->choice.Ok();   // S103 L9
  b->props_reader = w.light_shadow && b->choice.Ok();   // S103 RML
  EffectCoverage& cov = coverage_[mat.shader];
  cov.effect = mat.shader;
  if (b->choice.Ok()) {
    if (b->choice.layer_fallback) { ++layer_fallbacks_; cov.reason = "multi-layer pick fell back to one layer (counted)"; std::fprintf(stderr, "[front_end] layer fallback: %s technique %u (%d total)\n", mat.shader.c_str(), unsigned(mat.technique), layer_fallbacks_); }   // S103 LAY
    if (b->choice.vlm_fallback) cov.reason = "vertex-LM pick fell back to the per-pixel page pair (counted)";   // S103 L10: said in the coverage list
    if (cov.bound_to.empty()) {
      cov.bound_to = std::string(kVariants[b->choice.vs].name) + " + " + kVariants[b->choice.ps].name;
      for (uint32_t s = 0; s < kVariants[b->choice.ps].sampler_count; ++s)
        if (kVariants[b->choice.ps].samplers[s].slot == 8) cov.bound_to += (b->choice.tech_picked && w.cube && mat.reflect_alpha > 0.f) ? " [reflection: the file cube, g_fReflectAlpha]" : " [reflection: stand-in, ReflectAlpha 0]";  // S101 lane 15 / S103 RFL
    }
    b->program = ProgramFor(b->choice.vs, b->choice.ps, b->alpha_test);
    if (mat.stage_zw && VariantHasTwin(b->choice.ps)) b->zw_twin = true;   // S105 SZW
    b->layout = LayoutFor(b->choice.vs);
    const rdr::fx::Variant& fv = kVariants[b->choice.ps];
    for (uint32_t s = 0; s < fv.sampler_count; ++s) {
      const int slotn = fv.samplers[s].slot;
      rdr::ImageId image = white_;
      if (slotn == 0) image = w.diffuse ? tex(mat.diffuse) : white_;
      else if (slotn == 13) image = (std::getenv("WWE_RDR_L_NOLM") ? black_ : lm_image), b->slot_clamp[slotn] = 1;  // S103 L (dev diagnosis): WWE_RDR_L_NOLM=1 binds a black light map
      else if (slotn == 4) image = neutral_normal ? neutral_normal_ : tex(mat.normal);
      else if (slotn == 5) image = (tex(mat.specular) == rdr::kNoImage && b->choice.tech_picked) ? black_ : tex(mat.specular);   // S103 TQF: a table-picked spec-map variant with no spec sampler: black = no highlight ([U]: what the game binds)
      else if (slotn >= 1 && slotn <= 3) image = tex(mat.layer[slotn - 1]);   // S103 LAY
      else if (slotn == 6) image = tex(mat.noise) != rdr::kNoImage ? tex(mat.noise) : grey_;   // S105 C2L: sampNoisePrm = the file's grain picture (yProp_PSM, ring mat); none = grey
      else if (slotn == 8) {   // S101 lane 15: the reflection cube: black = no reflection layer. S103 RFL: the game's pick (the table's variant) is fed the model's own file cube + g_fReflectAlpha (Bound::cube_real -> Resolve ReflectAlpha)
        const rdr::ImageId fc = b->choice.tech_picked ? cube_of(mat.cube) : rdr::kNoImage;
        image = fc != rdr::kNoImage ? fc : black_cube_, b->cube_slot[slotn] = 3;
        b->cube_real = fc != rdr::kNoImage && mat.reflect_alpha > 0.f;
      }
      else if (slotn == 9) image = tex(mat.refmap);   // S103 RFL: the reflection-amount map (a variant that reads it was only chosen with the map present; kNoImage -> white below)
      else if (slotn == 12 && b->change_colour) image = tex(mat.change_mask[0]) != rdr::kNoImage ? tex(mat.change_mask[0]) : black_;   // S108 THB: the prop's mask (SPMT Mask1; black = no channel painted: the file diffuse)
      else if (slotn == 12 && (w.screen_shadow || w.light_shadow)) image = white_, b->cube_slot[slotn] = w.light_shadow ? 4 : 5;   // S103 RML: the light pass reads the props map (the floor's: rd_map at the draw)   // S103 L9: the frame's plane shadow map, set at the draw (white = no shadow)
      if (!options_.textures && slotn != 8) image = grey_;  // the planted fault: no textures
      b->slot_image[slotn] = image == rdr::kNoImage ? white_ : image;
    }
    if (!shadow) {   // S103 RFL census: a reflective material (the bound pixel variant samples the cube at slot 8) bound to the file cube / with none to feed; a counted fallback whose game variant is a Reflect one
      bool reads_cube = false;
      for (uint32_t s = 0; s < fv.sampler_count; ++s) reads_cube = reads_cube || fv.samplers[s].slot == 8;
      if (reads_cube) ++(b->cube_real ? reflect_binds_ : reflect_no_cube_);
      if (b->choice.tech_fallback && ReadsCube(b->choice.tech_ps)) {
        ++reflect_fallbacks_;
        rdr::Log("[scene][front_end] reflect fallback: %s technique %u: the game's variant %s is not bindable here (cube %d, amount map %d)", mat.shader.c_str(), unsigned(mat.technique), kVariants[b->choice.tech_ps].name, int(w.cube), int(w.refmap));
      }
    }
    {  // S103 L10: the vertex variant's textures (the page read in the vertex stage, slot 16: the SAME image the pixel stage reads at slot 13)
      const rdr::fx::Variant& vv = kVariants[b->choice.vs];
      for (uint32_t s = 0; s < vv.sampler_count; ++s) {
        const int slotn = vv.samplers[s].slot;
        if (slotn != 16) continue;
        const rdr::ImageId page = std::getenv("WWE_RDR_L_NOLM") ? black_ : lm_image;
        b->slot_image[slotn] = page == rdr::kNoImage ? white_ : page;
        b->slot_clamp[slotn] = 1;
        b->vtx_lm = true;
      }
    }
  } else {
    b->debug = true;
    if (std::getenv("WWE_RDR_DUMP_MATS")) std::fprintf(stderr, "[mats] UNBOUND %s tex=%s wants: diffuse=%d lightmap=%d normal=%d specmap=%d spec=%d atest=%d skinned=%d (material slots: normal=%d lm=%d specular=%d has_lm_slot=%d)\n", mat.shader.c_str(), mat.diffuse_name.c_str(), int(w.diffuse), int(w.lightmap), int(w.normal), int(w.specmap), int(w.spec), int(w.alpha_test), int(w.skinned), mat.normal, mat.lightmap, mat.specular, int(mat.has_lm_slot));  // lane 257 (dev)
    if (cov.reason.empty() && !w.screen_shadow && !w.light_shadow) cov.reason = b->choice.why_not;   // S103 L9: a failed plane bind falls back to the plain one
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
  const float* plane_m = nullptr; // S103 L9: the plane shadow map's projection (16 floats, register layout, in the draw's own space): PlaneInv = it, CameraViewProj = identity
  bool game_space = false;       // S103 lane D: the vertices, worlds and camera are the game's file space (a crowd class drawn instanced): the lights are fed as the file stores them
  const float* shadow_m = nullptr;    // S103 D1: an FX_SHADOW stage draw (the read rule's +2 pass): the frame's shadow map projection (MapImage::matrix, register layout, the draw's space)
  const float* shadow_prm = nullptr;  // S103 D1: MapImage::prm (ShadowPrm rows 0-2)
  bool plant_vs_prm_ps_rows = false;  // S103 RML planted twin (dev rig only): the VERTEX ShadowPrm block is fed the pixel rows again (the old behaviour: face-away term 0.2 x (1 - N.L) instead of 0)
  float mirror = 1.f;                 // S103 RFL: -1 when the draw's space is the viewer's (y negated), else 1
  const float* change_color = nullptr;   // S108 THB: an entrance prop's recoloured draw: ChangeColor (PS c16..c19) = these 4 vec4s ((R, G, B, 1) / 255, prop_colour.h ToConstants); null = zero (every other draw)
  bool dif_one = false;                  // S108 THB: the Threads editor's draw callback feeds MatDifCol (1, 1, 1, 1) over the file's own (sub_8338EC00: PS c190)
  const float* amb_rgb = nullptr;        // S111 CAARING: the object's own ambient colour (kObjAmbientColour: a created arena's rope, car_rope_colours.h): replaces the rgb of MatAmbCol; null = the material's
  float reflect_alpha = 0.f;          // S103 RFL: PS c208.x = the material's g_fReflectAlpha, set only for a draw whose slot 8 holds the model's own file cube (Bound::cube_real); 0 = the black stand-in's do-nothing value
};

// S103 RL: a camera-attached arena light (LightSet::follow, the u32 at +0x120 + 4 i of the 784-byte 30002 record) is fed as V^T d: d is VIEW space and the bound
// direction = right * d.x + down * d.y + forward * d.z, the frame camera's axes (forward = the clip-w column, down = minus the clip-y column orthonormalised, right =
// the clip-x column orthonormalised: char_effects.cpp KeyLightWorld's rule). The axes are taken from the draw's own ViewProj, so they are already in the draw's space
// (viewer space: no further Y flip; game space: as is). Game side: the flag words reach the light object at +1056 + 4 i (sub_827F70D0 0x827F717C-0x827F7184 and
// sub_827F8808 0x827F895C-0x827F8960 copy them from the source's +8960 + 4 i through sub_82417248); the model effect setter sub_82BE06B8 uploads DifLghVec from the installed record's +16
// (= object +64) as stored (S111 LIGHTS: its +960 / +1056 are slots of the model's [+832] block, not the light's) [C listings]; where the flagged directions are turned is not traced [U]; the rule is lane 33's measurement (V^T d,
// every flagged light 0.00 deg, docs\renderer\LIGHT_MATCH.md). The planted twin (the old fault, the stored direction unturned) is rf::c::Plants::no_arena_follow: LightsFor then leaves follow_view false.
void FollowCamera(const Ctx& c, const float* d, float* o) {
  if (!c.view_proj) return;
  double col[4][3];
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 3; ++j) col[k][j] = double(c.view_proj[j * 4 + k]);
  const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const auto unit = [&](double* a) {
    const double n = std::sqrt(dot(a, a));
    if (n > 1e-12)
      for (int j = 0; j < 3; ++j) a[j] /= n;
    return n > 1e-12;
  };
  double f[3] = {col[3][0], col[3][1], col[3][2]}, u[3], r[3];
  if (!unit(f)) return;
  const double uf = dot(col[1], f);
  for (int j = 0; j < 3; ++j) u[j] = col[1][j] - uf * f[j];
  if (!unit(u)) return;
  const double rf = dot(col[0], f), ru = dot(col[0], u);
  for (int j = 0; j < 3; ++j) r[j] = col[0][j] - rf * f[j] - ru * u[j];
  if (!unit(r)) return;
  for (int j = 0; j < 3; ++j) o[j] = float(r[j] * double(d[0]) - u[j] * double(d[1]) + f[j] * double(d[2]));
}

// S108 THB: what a recoloured entrance prop's draw feeds (the record's row of kCcProp -> SceneFrame::change_colours): ChangeColor = the 4 colours, MatDifCol (1, 1, 1, 1) under the Threads editor's callback (kCcEditor).
// The offline unit (tools\scene\thb_unit.cpp) calls this same function.
void FeedPropColour(Ctx& c, const float* constants, uint32_t flags) {
  c.change_color = constants;
  c.dif_one = (flags & kCcEditor) != 0;
}

// Fills `out` (vec4s * 4 floats, zeroed by the caller). False = the name is not one the front-end knows (reported once).
bool Resolve(const std::string& name, uint32_t vec4s, const Ctx& c, float* out, bool vs_stage = false /* S103 RML: the VERTEX stage's block (the stage decides, never the vec4 count) */) {
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
  if (name == "MatAmbCol") {
    std::memcpy(out, mt.ambient, 16);
    if (c.amb_rgb) for (int i = 0; i < 3; ++i) out[i] = c.amb_rgb[i];   // S111 CAARING: the game's sub_822A7F48 writes the rgb only (w stays the file's)
    return true;
  }
  if (name == "MatDifCol") {
    if (c.dif_one) { for (int i = 0; i < 4; ++i) out[i] = 1.f; }   // S108 THB: the editor's callback forces (1, 1, 1, 1)
    else std::memcpy(out, mt.diffuse_color, 16);
    return true;
  }
  if (name == "ChangeColor" && c.change_color) { std::memcpy(out, c.change_color, sizeof(float) * 4 * std::min<uint32_t>(vec4s, uint32_t(live::propcolour::kColours))); return true; }   // S108 THB
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
      out[4 * k] = ls.dir[k][0], out[4 * k + 1] = (c.game_space ? 1.f : -1.f) * ls.dir[k][1], out[4 * k + 2] = ls.dir[k][2];  // S103 lane D: game space = the file's own
      if (ls.follow_view && ls.follow[k]) FollowCamera(c, ls.dir[k], out + 4 * k);   // S103 RL: a camera-attached arena light
    }
    return true;
  }
  if (name == "FirstLightVec") {   // S103 TQ2 fix F5: the yDefault highlight's light = DifLghVec row 0, in the draw's space (viewer space: y negated)
    out[0] = ls.dir[0][0], out[1] = (c.game_space ? 1.f : -1.f) * ls.dir[0][1], out[2] = ls.dir[0][2];
    if (ls.follow_view && ls.follow[0]) FollowCamera(c, ls.dir[0], out);
    return true;
  }
  if (name == "vMin" || name == "fvMin" || name == "LightMin") return true;  // 0: no floor on n.l
  if (name == "SkyDir") { out[1] = c.game_space ? 1.f : -1.f; return true; }  // S103 lane D
  if (name == "SkyCol") { for (int i = 0; i < 3; ++i) out[i] = ls.sky[i]; out[3] = 1.f; return true; }
  if (name == "GroundCol") { for (int i = 0; i < 3; ++i) out[i] = ls.ground[i]; out[3] = 1.f; return true; }
  if (name == "LightMapParam") { out[0] = c.lm_xy[0]; out[1] = c.lm_xy[1]; out[2] = c.lm_gain; out[3] = 1.f; return true; }
  if (name == "EyePos") { out[0] = c.eye[0]; out[1] = c.eye[1]; out[2] = c.eye[2]; out[3] = 1.f; return true; }
  if (name == "Time") { out[0] = float(c.time); return true; }
  if (name == "SL_Scale") { if (!g_table_pick_all.load(std::memory_order_relaxed)) return false; out[0] = mt.sl_scale; out[1] = c.mirror; return true; }   // S103 TQ2 fix F3: .y = -1 in the viewer's y-negated space (fx_bg.glsl YZ branch flips world.y back)   // S103 TQ2 (UVS.md): the searchlight's planar-uv scale (VS c205) from the material's g_fSL_Scale; the dev plant leaves it unresolved (the old refusal)
  if (name == "UVScroll0" || name == "UVScroll1") {
    // S103 E6: the material's g_fScrollU/V.. (jboy.h -> Material::scroll): UVScroll0 = (U, V, U2, V2), UVScroll1 = (U3, V3, U4, V4). Absent (a scrolling class whose
    // record names none, or a material of another class asking) = 0, a still texture, counted and said once.
    if (mt.scroll_have) {
      const float* sp = mt.scroll + (name == "UVScroll1" ? 4 : 0);
      for (int i = 0; i < 4; ++i) out[i] = sp[i];
    } else if (mt.shader.find("UVScrol") != std::string::npos) {
      static std::atomic<uint32_t> absent{0};
      if (absent.fetch_add(1) == 0) std::fprintf(stderr, "[uvscroll absent: a scrolling material (%s) has no g_fScroll parameter: speeds 0]\n", mt.shader.c_str());
    }
    return true;
  }
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
  if (c.plane_m && name == "PlaneInv") {   // S103 L9: rows 0, 1 and 3 are read (the vertex stage skips row 2)
    for (uint32_t k = 0; k < 4 && k < vec4s; ++k)
      for (int i = 0; i < 4; ++i) out[4 * k + uint32_t(i)] = c.plane_m[4 * k + uint32_t(i)];
    return true;
  }
  if (c.plane_m && name == "CameraViewProj") {   // S103 L9: identity: the plane map's matrix already holds PlaneInv x CameraViewProj
    for (uint32_t k = 0; k < 4 && k < vec4s; ++k) out[4 * k + k] = 1.f;
    return true;
  }
  if (c.shadow_m && c.shadow_prm) {   // S103 D1: the yBG / yProp FX_SHADOW receiver (fx_bg.glsl: TexProjLight rows 0, 1, 3 project, ViewLight row 2 = the light depth row; the same rows the
    // character reader takes: TexProjLightXY = rows 0-1, TexProjLightW = ViewLightDepth = row 3, char_effects.cpp "S103 L6" [C there]). ViewLight rows 0-1 are not read for the shadow: zero.
    if (name == "TexProjLight") { std::memcpy(out, c.shadow_m, sizeof(float) * 4 * std::min<uint32_t>(vec4s, 4)); return true; }
    if (name == "ViewLight") { if (vec4s >= 3) std::memcpy(out + 8, c.shadow_m + 12, 16); return true; }
    if (name == "ShadowPrm") {
      // S103 RML (RMP.md 3 [C wm30 f0 / f1, z7147 f100: the game's VS c168-c169 = (0, 0, 0, 1) x 2 on every receiver): the VERTEX block of a receiver holds the constant pair (0, 0, 0, 1) x 2, NOT the pixel rows
      // (the vertex face-away term ShadowPrm[1].y is then 0, not 0.2: the stair risers / walls / apron fold keep their light). The PIXEL block (3 rows) is the props map's prm.
      if (vs_stage && !c.plant_vs_prm_ps_rows) { std::fill(out, out + 4 * std::min<uint32_t>(vec4s, 2), 0.f); for (uint32_t k = 0; k < vec4s && k < 2; ++k) out[4 * k + 3] = 1.f; }
      else std::memcpy(out, c.shadow_prm, sizeof(float) * 4 * std::min<uint32_t>(vec4s, 3));
      return true;
    }
  }
  if (name == "SpeLodParam") { out[0] = mt.spe_lod[0], out[1] = mt.spe_lod[1] > mt.spe_lod[0] ? mt.spe_lod[1] : 1e30f; return true; }   // S103 D1: FX_SPEC_FADE (the read rule picks it: barricade technique 15): the material's g_fSpeLodNear / Far [C: run29 f500 game y = 6000 = the file's far]; none / a bad range = no fade ([U] default), never 0 / 0
  if (name == "ReflectAlpha") { out[0] = c.reflect_alpha; out[1] = c.mirror; return true; }   // .y = -1 in the viewer's Y-mirrored space (fx_bg / fx_prop flip the cube lookup)   // S103 RFL: g_fReflectAlpha [C: RFF.md section A, c208.x of every Reflect draw read] when the file cube is bound, else 0
  if (name == "Noise" || name == "NoiseParam") {   // S105 C2L (C2L.md 3b [C]: the game's name entries pack g_iNoiseTileX / g_iNoiseTileY / g_fNoiseLvl into c207.xyz): PS `Noise` and VS `NoiseParam` read (tile x, tile y, level); a material that names none stays zero
    if (vec4s >= 1) out[0] = mt.noise_tile[0], out[1] = mt.noise_tile[1], out[2] = mt.noise_lvl;
    return true;
  }
  if (name == "ShadowPrm" || name == "CameraViewProj" || name == "PlaneInv" ||
      name == "TexProjLight" || name == "ViewLight" || name == "ConvScreen" || name == "SpeLodParam" || name == "ChangeColor")
    return true;  // zero: the option that reads them is not chosen, or the value is the neutral one
  return false;
}

}  // namespace

// S103 D1: a stage-slot mesh bound to the pair the READ RULE chose (read_rule.h: effect alternative, technique, pass -> the pass's shaders by their microcode hash). Textures go by the
// pair's own sampler slots, as BindMaterial does: 0 diffuse, 13 the light map (the stage page, else the mesh's own), 4 normal, 5 specular map, 6 grey, 8 the black cube, 12 the frame's
// shadow map (FX_SHADOW) or plane map (FX_SCREEN_SHADOW), vertex 16 the page. nullptr = the pair needs something the front end cannot feed (the caller keeps the old path, counted).
const FrontEnd::Bound* FrontEnd::BindRule(ModelData& m, int material, bool skinned, int vs, int ps, bool shadow) {
  if (material < 0 || vs < 0 || ps < 0 || vs >= kVariantCount || ps >= kVariantCount || !T().by_index[size_t(vs)] || !T().by_index[size_t(ps)]) return nullptr;
  const uint64_t key = (uint64_t(uint32_t(material) + 1) << 42) | (uint64_t(skinned) << 41) | (uint64_t(shadow) << 40) | (uint64_t(uint32_t(vs)) << 20) | uint64_t(uint32_t(ps));
  if (auto it = m.rule_bound.find(key); it != m.rule_bound.end()) return it->second.get();
  const fxt::VariantTable& vt = *T().by_index[size_t(vs)];
  const fxt::VariantTable& ft = *T().by_index[size_t(ps)];
  const rdr::fx::Variant& vv = kVariants[vs];
  const rdr::fx::Variant& fv = kVariants[ps];
  const auto layer_ok = [&](int i) { return i >= 0 && size_t(i) < m.images.size() && m.images[size_t(i)] != rdr::kNoImage; };   // S103 LAY: a failed upload must not bind white_
  bool ok = HasDefine(vt, "FX_SKINNED") == skinned;
  for (uint32_t k = 0; ok && k < vt.input_count; ++k) ok = SupportedInput(vt.inputs[k].name, skinned);
  for (uint32_t s = 0; ok && s < vv.sampler_count; ++s) ok = vv.samplers[s].slot == 16 && vv.samplers[s].kind == 0;
  if (options_.plant_one_layer && (HasDefine(ft, "FX_LAYER2") || HasDefine(ft, "FX_LAYER3") || HasDefine(ft, "FX_LAYER4"))) ok = false;   // S103 LAY planted twin: the old one-layer draw (the rule's layer pair is refused; the old path then takes the one-layer variant, UvLayers)
  const bool reads_shadow = HasDefine(ft, "FX_SHADOW"), reads_plane = HasDefine(ft, "FX_SCREEN_SHADOW");
  for (uint32_t s = 0; ok && s < fv.sampler_count; ++s) {
    const int sl = fv.samplers[s].slot;
    ok = (fv.samplers[s].kind == 0 && (sl == 0 || sl == 4 || sl == 5 || sl == 6 || sl == 13 || (sl >= 1 && sl <= 3 && layer_ok(m.materials[size_t(material)].layer[sl - 1])) || (sl == 12 && (reads_shadow || reads_plane)) || (sl == 9 && g_reflections.load(std::memory_order_relaxed) && layer_ok(m.materials[size_t(material)].refmap)))) || (sl == 8 && fv.samplers[s].kind == 1);   // S103 RFL: slot 9 = the reflection-amount map, when the material has one
  }
  for (uint32_t k = 0; ok && k < ft.interp_count; ++k) {   // every interpolator the fragment reads is written by the vertex stage at the same location
    bool found = false;
    for (uint32_t j = 0; j < vt.interp_count; ++j) found = found || vt.interps[j].location == ft.interps[k].location;
    ok = found;
  }
  const assets::Material& mat = m.materials[size_t(material)];
  const auto tex = [&](int i) -> rdr::ImageId { return (i >= 0 && size_t(i) < m.images.size() && !(size_t(i) < m.img_info.size() && m.img_info[size_t(i)].faces == 6)) ? m.images[size_t(i)] : rdr::kNoImage; };   // S103 RFL: a cube image never reaches a 2D slot
  const rdr::ImageId stage = m.stage_lm >= 0 ? tex(m.stage_lm) : rdr::kNoImage;
  const rdr::ImageId lm_image = stage != rdr::kNoImage ? stage : tex(mat.lightmap);
  {  // review 3: a pair that reads the page (pixel slot 13 / vertex slot 16) with no page image would read white x LightMapParam.z: the old path instead
    bool reads_page = vv.sampler_count > 0;
    for (uint32_t s = 0; s < fv.sampler_count; ++s) reads_page = reads_page || fv.samplers[s].slot == 13;
    if (reads_page && lm_image == rdr::kNoImage) ok = false;
  }
  if (ok) {  // review 2: the old path's Unsupported() filter, by what we can feed: every constant field of both stages must be one Resolve knows (a dry run; values unused)
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const float zero3[3] = {0, 0, 0}, xy[2] = {0, 0}, prm[12] = {};
    static const assets::LightSet rig = assets::DefaultRig();
    Ctx dry{ident, ident, zero3, 0.0, &mat, &rig, xy, 1.f, 1.f};
    dry.shadow_m = ident, dry.shadow_prm = prm;
    static const assets::Matrix one_bone = assets::Identity();
    if (skinned) dry.bones = &one_bone, dry.bone_count = 1;   // (the draw feeds the chunk's palette: Bools / WorldMatrixArray take the skinned answers)
    float scratch[4 * 256];
    for (const fxt::VariantTable* t : {&vt, &ft})
      for (uint32_t k = 0; ok && k < t->field_count; ++k) {
        if (t->fields[k].vec4s == 0 || t->fields[k].vec4s > 256) continue;
        std::fill(scratch, scratch + 4 * t->fields[k].vec4s, 0.f);
        ok = Resolve(t->fields[k].name, t->fields[k].vec4s, dry, scratch);
      }
  }
  if (!ok || (reads_shadow && !shadow)) { m.rule_bound.emplace(key, nullptr); return nullptr; }
  auto b = std::make_unique<Bound>();
  b->choice.vs = vs, b->choice.ps = ps, b->choice.effect = mat.shader;
  b->alpha_test = (mat.alpha_test || GameTestsAlpha(mat, ps, ps, options_.plant_untested_alpha)) && fv.bytes_alpha != 0;   // S103 B2 (TQF: by the variant index)
  b->shadow = reads_shadow;
  b->plane = reads_plane;
  b->program = ProgramFor(vs, ps, b->alpha_test);
  if (mat.stage_zw && VariantHasTwin(ps)) b->zw_twin = true;   // S105 SZW
  b->layout = LayoutFor(vs);
  const bool neutral_normal = tex(mat.normal) == rdr::kNoImage && neutral_normal_ != rdr::kNoImage;
  for (uint32_t s = 0; s < fv.sampler_count; ++s) {
    const int slotn = fv.samplers[s].slot;
    rdr::ImageId image = white_;
    if (slotn == 0) image = tex(mat.diffuse) != rdr::kNoImage ? tex(mat.diffuse) : white_;
    else if (slotn == 13) image = lm_image, b->slot_clamp[slotn] = 1;
    else if (slotn == 4) image = neutral_normal ? neutral_normal_ : tex(mat.normal);
    else if (slotn == 5) image = tex(mat.specular);
    else if (slotn >= 1 && slotn <= 3) image = tex(mat.layer[slotn - 1]);   // S103 LAY
    else if (slotn == 6) image = tex(mat.noise) != rdr::kNoImage ? tex(mat.noise) : grey_;   // S105 C2L: sampNoisePrm = the file's grain picture (yProp_PSM, ring mat); none = grey
    else if (slotn == 8) {   // S103 RFL: the model's own file cube (the pair is the game's: the read rule's table row), else the black stand-in
      const int ci = mat.cube;
      const bool real = g_reflections.load(std::memory_order_relaxed) && !options_.plant_cube_unbound && ci >= 0 && size_t(ci) < m.images.size() && size_t(ci) < m.img_info.size() && m.img_info[size_t(ci)].faces == 6 && m.images[size_t(ci)] != rdr::kNoImage;
      image = real ? m.images[size_t(ci)] : black_cube_, b->cube_slot[slotn] = 3;
      b->cube_real = real && mat.reflect_alpha > 0.f;
      ++(b->cube_real ? reflect_binds_ : reflect_no_cube_);
    }
    else if (slotn == 9) image = tex(mat.refmap);   // S103 RFL
    else if (slotn == 12) image = white_, b->cube_slot[slotn] = reads_shadow ? 4 : 5;   // the frame's shadow / plane map, set at the draw
    if (!options_.textures && slotn != 8) image = grey_;
    b->slot_image[slotn] = image == rdr::kNoImage ? white_ : image;
  }
  for (uint32_t s = 0; s < vv.sampler_count; ++s) {
    const int slotn = vv.samplers[s].slot;
    b->slot_image[slotn] = lm_image == rdr::kNoImage ? white_ : lm_image;
    b->slot_clamp[slotn] = 1;
    b->vtx_lm = true;
  }
  EffectCoverage& cov = coverage_[mat.shader];
  cov.effect = mat.shader;
  if (cov.bound_to.empty()) cov.bound_to = std::string(vv.name) + " + " + fv.name + " [read rule]";
  const Bound* out = b.get();
  m.rule_bound.emplace(key, std::move(b));
  return out;
}

// S103 RFL: the reflection census line: the file cubes loaded (name + size), the materials bound to one (and those that had none to feed), the draws that sampled a real cube, and the counted fallbacks
// (the game's Reflect variant that could not be bound). Printed on the live census line; the offline rigs print it too.
std::string FrontEnd::WrkCensusLine() const {   // S103 WRK: no newline inside (the [scene][draw] line)
  const WrkCensus& w = wrk_;
  auto n = [](uint64_t v) { return std::to_string((unsigned long long)v); };
  std::string s = std::string("char_pass_mode m0 ") + n(w.m0) + " m2 " + n(w.m2) + " mo " + n(w.m_other) + " mu " + n(w.m_unknown) + "; m620 v0 " + n(w.m620[0]) + " v1 " + n(w.m620[1]) + " v2 " + n(w.m620[2]) + " v3 " + n(w.m620[3]) + " vu " + n(w.m620[4]) +
                  "; e20 set " + n(w.e20_set) + " clear " + n(w.e20_clear) + " unknown " + n(w.e20_unknown) + " plus16_would_fire " + n(w.e20_would_fire) + " (not used for the pick)";
  s += std::string(" wrk ") + (wrk::Live() ? "on" : "off") + ": wrinkle_draws " + n(w.wrinkle_draws) + " levels_missing " + n(w.levels_missing) + " wkl_nonzero " + n(w.wkl_nonzero) + " idmap_nonzero " + n(w.idmap_nonzero) + " wrinkle_map_fallback_binds " + n(w.slot_fallback) + " uv_degenerate " + n(w.uv_degenerate);
  return s;
}

std::string FrontEnd::ReflectLine() const {
  std::string s = "reflect (S103 RFL) " + std::string(g_reflections.load(std::memory_order_relaxed) ? "on" : "off") + ": cubes loaded " + std::to_string(reflect_cubes_);
  if (!reflect_cube_names_.empty()) s += " [" + reflect_cube_names_ + "]";
  s += ", yBG_Reflect table picks " + std::to_string(bgrefl_picked_) + " / fallbacks " + std::to_string(bgrefl_fallbacks_);
  s += ", materials bound to the file cube " + std::to_string(reflect_binds_) + ", with no cube " + std::to_string(reflect_no_cube_) + ", reflect draws " + std::to_string((unsigned long long)reflect_draws_) + ", fallbacks " + std::to_string(reflect_fallbacks_);
  return s;
}

std::string FrontEnd::FlashLine() const {   // S110 RDRPREP F3
  char near_txt[96];   // S110 FLASHOCC: near = a centre nearer than any crowd flash seen from inside the ring (kFlashNearViewZ); nearest = the smallest centre view z drawn
  std::snprintf(near_txt, sizeof near_txt, " near(view z < %.0f) %llu nearest %.0f", double(kFlashNearViewZ), (unsigned long long)flash_near_, double(flash_nearest_));
  const auto n = [](uint64_t v) { return std::to_string((unsigned long long)v); };
  return "flashes (S110): given " + n(flash_given_) + " drawn " + n(flash_drawn_) + " nodepth " + n(flash_nodepth_) + near_txt + " keep (S111 FLASHCAM): frames " + n(fc_frames_) + " sprites " + n(fc_sprites_) +
         " off_picture " + n(fc_off_picture_) + " game_kept " + n(fc_game_kept_) + " game_refused " + n(fc_game_refused_) + " game_unknown " + n(fc_game_unknown_) + " kept_off_picture " + n(fc_kept_off_);
}

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
  const uint64_t tick = ++cube_tick_;
  const auto found = cubes_.find(key);
  if (found != cubes_.end()) return found->second.used = tick, found->second;
  if (cubes_.size() >= 32) {  // S103 E8: LRU, not a wipe. A wrestler's own light state moves with the camera, so new keys arrive every few frames (run 28: 76 distinct over 291 records); the old WaitIdle + destroy-all stalled the live frame. DestroyImage is deferred by the renderer (kDestroyDelayFrames): nothing waits.
    auto oldest = cubes_.begin();
    for (auto it = cubes_.begin(); it != cubes_.end(); ++it)
      if (it->second.used < oldest->second.used) oldest = it;
    if (oldest->second.image != rdr::kNoImage) renderer_.DestroyImage(oldest->second.image);
    cubes_.erase(oldest);
    ++cube_evicted_;
  }
  ++cube_built_;
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
  e.used = tick;
  return cubes_.emplace(key, e).first->second;
}

// WGS PAR1 (TEST ONLY: tools\scene\frame_rig.cpp --prime-grab for tools\scene\arena_sheet.py; the game never calls it): key 800 (and an idle movie key) samples this picture through the
// grab's own binding instead of the copy of the previous frame, and RenderToPixels draws no seed frame. A null picture clears it (the live copy again).
void FrontEnd::PrimeGrabForTest(const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (grab_prime_ != rdr::kNoImage && (!rgba || w != grab_prime_w_ || h != grab_prime_h_)) renderer_.DestroyImage(grab_prime_), grab_prime_ = rdr::kNoImage;
  if (!rgba || !w || !h) return;
  if (grab_prime_ == rdr::kNoImage) {
    rdr::ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h);
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    grab_prime_ = renderer_.CreateImage(d);
    grab_prime_w_ = w, grab_prime_h_ = h;
  }
  if (grab_prime_ != rdr::kNoImage) renderer_.UpdateImage(grab_prime_, 0, 0, rgba, size_t(w) * h * 4);
}

bool FrontEnd::ReadDepthForTest(std::vector<uint8_t>& raw, uint32_t& w, uint32_t& h) {   // SCU (TEST ONLY, front_end.h)
  if (depth_ == rdr::kNoImage || !depth_stored_) return false;   // a transient depth is not stored by its pass: nothing to read
  rdr::ImageRef ref;
  ref.image = depth_;
  return renderer_.ReadImage(ref, raw, w, h);
}

namespace {
// S105 A2: the big screen's grab is the game's own 16:9 view = the centred box of a scene made wider (Hor+) or taller (Vert+) than 16:9 (wide_frame.h BoxOf); the whole target at 16:9.
using BigScreenBox = wwe2k14::scene_host::wide_frame::Box;
BigScreenBox BigScreenBoxOf(uint32_t w, uint32_t h) { return wwe2k14::scene_host::wide_frame::BoxOf(w, h); }
}  // namespace

void FrontEnd::EnsureTargets(uint32_t w, uint32_t h, bool wait_idle) {
  // S105 A1 review F2: every target must be there (a colour made and a depth / post output that failed was never retried: every frame "Record without targets")
  if (color_ != rdr::kNoImage && depth_ != rdr::kNoImage && (!depth_sampled_ || post_out_ != rdr::kNoImage) && target_w_ == w && target_h_ == h && depth_sampled_ == want_depth_sampled_ && depth_stored_ == want_depth_stored_ && targets_scaled_ == scaled_targets_) return;  // S101 lane 34: a sampled (non-transient) depth when the post pass reads it
  if (wait_idle) renderer_.WaitIdle();   // S103 P: not on the live path: DestroyImage below is deferred (renderer.cpp kDestroyDelayFrames = 12 frames past the last submit that could use the image), nothing of THIS frame has recorded a use yet
  depth_sampled_ = want_depth_sampled_, depth_stored_ = want_depth_stored_;
  if (color_ != rdr::kNoImage) renderer_.DestroyImage(color_);
  if (depth_ != rdr::kNoImage) renderer_.DestroyImage(depth_);
  if (post_out_ != rdr::kNoImage) renderer_.DestroyImage(post_out_), post_out_ = rdr::kNoImage;
  if (live_screen_ != rdr::kNoImage) renderer_.DestroyImage(live_screen_), live_screen_ = rdr::kNoImage;  // S101 lane 257
  { LiveScreenFacts lf; lf.size_changed = true; InvalidateLiveScreen(LiveScreenInvalidation(lf)); }   // S103 SCX: the grab's size changed (the one invalidation that is not a scene change)
  targets_scaled_ = scaled_targets_;
  for (int attempt = 0; attempt < 2; ++attempt) {
    rdr::ImageDesc c;
    c.width = uint16_t(w);
    c.height = uint16_t(h);
    c.format = rdr::Format::RGBA8;
    const uint8_t scaled_bit = targets_scaled_ ? uint8_t(rdr::kUsageScaled) : uint8_t(0);   // S105 A1: colour + depth + the grab + the post output are screen images: drawn at the player's size (a pass is the smallest of its targets, so colour and depth must agree)
    c.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageSampled | scaled_bit);
    color_ = renderer_.CreateImage(c);
    {   // S101 lane 257: the big screen's copy of the last frame. S105 A2: the game's own 16:9 grab, the centred box of a wider / taller scene (the whole target at 16:9)
      const BigScreenBox bx = BigScreenBoxOf(w, h);
      rdr::ImageDesc sd = c;
      sd.width = uint16_t(bx.w), sd.height = uint16_t(bx.h);
      live_screen_ = renderer_.CreateImage(sd);
    }
    rdr::ImageDesc d = c;
    d.format = rdr::Format::Depth;
    d.usage = uint8_t(((depth_sampled_ || depth_stored_) ? uint8_t(rdr::kUsageTarget | rdr::kUsageSampled) : uint8_t(rdr::kUsageTarget | rdr::kUsageTransient)) | scaled_bit);
    depth_ = renderer_.CreateImage(d);
    if (targets_scaled_ && g_a1_plant_scaled_alloc_fail.load(std::memory_order_relaxed) && depth_ != rdr::kNoImage) renderer_.DestroyImage(depth_), depth_ = rdr::kNoImage;   // RIG-ONLY planted failure (F2)
    if (depth_sampled_) post_out_ = renderer_.CreateImage(c);  // S101 lane 34: the post pass's output (the scene colour's description)
    const bool ok = color_ != rdr::kNoImage && depth_ != rdr::kNoImage && live_screen_ != rdr::kNoImage && (!depth_sampled_ || post_out_ != rdr::kNoImage);
    if (ok || !targets_scaled_ || attempt == 1) break;
    // S105 A1 review F2: a scaled target could not be made (4K memory): log once and fall back to unscaled targets (the picture is then the 720p one blitted up, as before the fix), never a black 3D for the run
    static bool logged_fallback = false;
    if (!logged_fallback) logged_fallback = true, rdr::Log("[scene][targets] could not make the scaled scene targets at logical %ux%u (colour %d, depth %d, grab %d, post output %d): falling back to unscaled targets", w, h, int(color_ != rdr::kNoImage), int(depth_ != rdr::kNoImage), int(live_screen_ != rdr::kNoImage), int(!depth_sampled_ || post_out_ != rdr::kNoImage));
    if (color_ != rdr::kNoImage) renderer_.DestroyImage(color_), color_ = rdr::kNoImage;
    if (depth_ != rdr::kNoImage) renderer_.DestroyImage(depth_), depth_ = rdr::kNoImage;
    if (post_out_ != rdr::kNoImage) renderer_.DestroyImage(post_out_), post_out_ = rdr::kNoImage;
    if (live_screen_ != rdr::kNoImage) renderer_.DestroyImage(live_screen_), live_screen_ = rdr::kNoImage;
    targets_scaled_ = false;
  }
  // S103 GT: the GPU role of each scene image (the per-pass stamps' table; the stream-free host has no capture records to name them from): the scene colour + depth = colour, the post output = post, the big screen's copy = copies
  // (only in the stream-free build: in the stream build a class also steers the binder's per-frame levers, and its capture images are named from the records)
#if !WWE2K14_STREAM()
  if (color_ != rdr::kNoImage) renderer_.SetImageClass(color_, rdr::kGpuColor);
  if (depth_ != rdr::kNoImage) renderer_.SetImageClass(depth_, rdr::kGpuColor);
  if (post_out_ != rdr::kNoImage) renderer_.SetImageClass(post_out_, rdr::kGpuPost);
  if (live_screen_ != rdr::kNoImage) renderer_.SetImageClass(live_screen_, rdr::kGpuCopies);
#endif
  target_w_ = w, target_h_ = h;
  {   // S105 A1: the REAL created sizes, once per (re)make: a check reads them (a scaled scene at 1440p prints 2560x1440 for colour and depth; an ignored choice prints 1280x720 with scaled=0)
    uint32_t cw = 0, ch = 0, dw = 0, dh = 0;
    bool cs = false, ds = false;
    renderer_.ImageExtent(color_, cw, ch, cs), renderer_.ImageExtent(depth_, dw, dh, ds);
    if (logged_target_w_ != w || logged_target_h_ != h || logged_target_scaled_ != int(cs)) {   // S105 A1 review F5: only a new size / flag (the stream's shared pass alternates sizes)
      logged_target_w_ = w, logged_target_h_ = h, logged_target_scaled_ = int(cs);
      rdr::Log("[scene][targets] logical %ux%u: colour %ux%u scaled=%d, depth %ux%u scaled=%d (the post chain's depth image and the soft-particle depth log theirs when made)", w, h, cw, ch, int(cs), dw, dh, int(ds));
    }
  }
}

// S103 lane S: see front_end.h.
LiveTargets FrontEnd::OwnTargets(uint32_t w, uint32_t h, bool post, bool keep_depth) {
  want_depth_stored_ = want_depth_stored_ || keep_depth;
  want_depth_sampled_ = want_depth_sampled_ || post;   // S103 P: sticky upward: a post / no-post flip between frames must not remake every target
  EnsureTargets(w, h, /*wait_idle=*/false);
  LiveTargets t;
  t.color = color_, t.depth = depth_, t.width = w, t.height = h;
  return t;
}

// S101 lane24: the programs are made again at the next draw (the GPU is idle first: the old pipelines may still be in flight).
void FrontEnd::ReloadPrograms(const std::vector<int>& changed) {
  renderer_.WaitIdle();
  if (draw_lists_) draw_lists_->Clear();  // S103 G: bindings are made again
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
    if (m) {
      for (auto& b : m->bound) b.reset();
      m->char_bound.clear();   // S103 YCB
      m->rule_bound.clear(), m->rule_pick.clear();   // review 4 (S103 D1)
    }
}

bool FrontEnd::RenderToPixels(const SceneFrame& frame, std::vector<uint8_t>& rgba, std::string& error, uint32_t* out_w, uint32_t* out_h) {
  if (ShaderReload* reload = ShaderReload::Get())  // S101 lane24: adopts a finished reload before the frame is built
    if (std::vector<int> changed; reload->Poll(&changed)) ReloadPrograms(changed);
  if (frame.width == 0 || frame.height == 0 || frame.width > 16384 || frame.height > 16384) { error = "rdr: bad frame size"; return false; }
  // S101 lane 257: a match-look frame with the stage's big screen in it first draws one seed frame (no post), so the screen shows the picture of this scene, not black (the viewer's rule)
  bool have_screen = false;
  static const bool no_live_screen = std::getenv("WWE_RDR_NO_LIVE_SCREEN") != nullptr;  // lane 257 plant: the screen stays black (the census must fail)
  if (!no_live_screen)   // S103 SCF: key 800 draws the frame grab in EVERY look (the game: channel -1 = mode 1, sub_8276A808; no look test in sub_8276AEF8)
    for (const Object& obj : frame.objects) {
      if (!(obj.flags & kObjVisible) || obj.asset >= models_.size() || !models_[obj.asset]->live) continue;
      have_screen = have_screen || models_[obj.asset]->idle_screens != 0;   // S103 SCF: the one pre-post scene copy also feeds every shown idle movie-key slot
      for (const ModelData::Mesh& me : models_[obj.asset]->meshes) have_screen = have_screen || me.part == "slot 800";
    }
  if (have_screen && !in_seed_ && grab_prime_ == rdr::kNoImage) {   // WGS PAR1: a primed grab (TEST ONLY) needs no seed frame (the seed would be what the grab shows otherwise)
    in_seed_ = true;
    SceneFrame seed = frame;
    seed.post = nullptr;
    std::vector<uint8_t> ignored;
    uint32_t seed_w = 0, seed_h = 0;
    const bool ok = RenderToPixels(seed, ignored, error, &seed_w, &seed_h);
    in_seed_ = false;
    if (!ok) return false;
  }
  want_depth_sampled_ = want_depth_sampled_ || frame.post != nullptr || !frame.flashes.empty(), want_depth_stored_ = want_depth_stored_ || LiveParticles::HasSoft(frame.particles);  // S101 lane 34; S103 P: sticky upward (the mirror's seed frame has no post and must not flip the targets); S110 CUTGREY: the flash sprites sample the scene depth at their centre
  EnsureTargets(frame.width, frame.height);
  if (color_ == rdr::kNoImage || depth_ == rdr::kNoImage) { error = "rdr: could not create the frame's images"; return false; }

  // S103 lane S: the frame is recorded by Record() into the front-end's own images, then read back
  LiveTargets own;
  own.color = color_, own.depth = depth_, own.width = frame.width, own.height = frame.height;
  builder_.Begin(0);
  if (!Record(frame, builder_, own, error)) return false;
  renderer_.RenderFrame(builder_.Finish());
  renderer_.WaitIdle();
  uint32_t w = 0, h = 0;
  rdr::ImageRef ref;
  ref.image = frame.post ? post_out_ : color_;
  if (!renderer_.ReadImage(ref, rgba, w, h)) { error = "rdr: could not read the frame back"; return false; }
  if (out_w && out_h) *out_w = w, *out_h = h;   // S105 A1: a scaled scene reads back at its real size
  else if (w != frame.width || h != frame.height) { error = "rdr: could not read the frame back"; return false; }
  return true;
}

bool FrontEnd::RenderNoWait(const SceneFrame& frame, uint32_t renders, std::string& error) {   // S103 PF2 (RIG ONLY)
  if (frame.width == 0 || frame.height == 0) { error = "rdr: bad frame size"; return false; }
  want_depth_sampled_ = want_depth_sampled_ || frame.post != nullptr, want_depth_stored_ = want_depth_stored_ || LiveParticles::HasSoft(frame.particles);
  EnsureTargets(frame.width, frame.height);
  if (color_ == rdr::kNoImage || depth_ == rdr::kNoImage) { error = "rdr: could not create the frame's images"; return false; }
  LiveTargets own;
  own.color = color_, own.depth = depth_, own.width = frame.width, own.height = frame.height;
  builder_.Begin(0);
  if (!Record(frame, builder_, own, error)) return false;
  const rdr::Frame built = builder_.Finish();
  for (uint32_t i = 0; i < renders; ++i) renderer_.RenderFrame(built);
  return true;
}

// S103 CB1 (RIG ONLY): the frame is recorded (every stage hook runs, so a hook that keeps GPU state across frames believes it) and then thrown away before RenderFrame, as a host that builds a frame and does not hand it over;
// note_dropped = the host tells the renderer (Renderer::NoteFrameDropped, scene_host.cpp). false = the planted twin of a host that forgets.
bool FrontEnd::RecordAndDiscard(const SceneFrame& frame, bool note_dropped, std::string& error) {
  if (frame.width == 0 || frame.height == 0) { error = "rdr: bad frame size"; return false; }
  want_depth_sampled_ = want_depth_sampled_ || frame.post != nullptr, want_depth_stored_ = want_depth_stored_ || LiveParticles::HasSoft(frame.particles);
  EnsureTargets(frame.width, frame.height);
  if (color_ == rdr::kNoImage || depth_ == rdr::kNoImage) { error = "rdr: could not create the frame's images"; return false; }
  LiveTargets own;
  own.color = color_, own.depth = depth_, own.width = frame.width, own.height = frame.height;
  builder_.Begin(0);
  if (!Record(frame, builder_, own, error)) return false;
  (void)builder_.Finish();   // never rendered
  if (note_dropped) renderer_.NoteFrameDropped();
  return true;
}

// S103 lane S (FRAME_CONTRACT.md 8): the frame recorded into the caller's builder and targets; never begins, finishes, waits or reads back.
// S103 B3: a card-technique model's gain = the table's gain x the card factor of this frame; the game applies the factor only when > 0 (no row / 0 = the gain unscaled, counted per Record).
void FrontEnd::ApplyLmRow(ModelData& m) { m.lm_gain = card_gain::Gain(m.lm_base_gain, m.lm_card, card_factor_); }
void FrontEnd::SetCardFactor(bool valid, float factor) {
  card_factor_ = card_gain::Effective(valid, factor);
  if (card_factor_ <= 0.f) ++card_factor_missing_;
}

bool FrontEnd::Record(const SceneFrame& frame, rdr::FrameBuilder& fb, const LiveTargets& tg, std::string& error) {
  struct RingNote { FrontEnd* fe; const rdr::FrameBuilder& b; ~RingNote() { fe->NoteRingFrame(b); } } ring_note{this, fb};   // S103 PVR: every return path tells the posed budget what the other producers pushed
  SetCardFactor(frame.card_factor_valid, frame.card_factor);   // S103 B3: before any draw of this frame (the crowd's DrawInstanced follows in the same frame)
  if (tg.color == rdr::kNoImage || tg.depth == rdr::kNoImage || tg.width == 0 || tg.height == 0) { error = "rdr: Record without targets"; return false; }
  if (FILE* fdf = fdump::File()) fdump::Pass(fdf, "\"name\":\"frame_begin\"");   // S103 T2 (dev): one line per Record (the rig draws the whole frame, then each family's panel: the tool reads the first)
  bool have_screen = false;  // S101 lane 257 (as in RenderToPixels): the stage's big screen copies this frame's colour for the next frame
  if (std::getenv("WWE_RDR_NO_LIVE_SCREEN") == nullptr)   // S103 SCF: every look (see RenderToPixels)
    for (const Object& obj : frame.objects) {
      if (!(obj.flags & kObjVisible) || obj.asset >= models_.size() || !models_[obj.asset]->live) continue;
      have_screen = have_screen || models_[obj.asset]->idle_screens != 0;   // S103 SCF: the one pre-post scene copy also feeds every shown idle movie-key slot
      for (const ModelData::Mesh& me : models_[obj.asset]->meshes) have_screen = have_screen || me.part == "slot 800";
    }
  if (have_screen && live_screen_ == rdr::kNoImage) have_screen = false;  // (the live caller has no seed frame: the screen starts from the next frame)
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

  // S103 lane G (SCENE_PACING.md 3b): the kept draw lists. BeginFrame / EndFrame bracket every return of this function; the stamps below are what a draw
  // compares with its last frame's (null lists = every field resolved from scratch, as before).
  struct ListsFrame {
    DrawLists* l;
    explicit ListsFrame(DrawLists* x) : l(x) { if (l) l->BeginFrame(); }
    ~ListsFrame() { if (l) l->EndFrame(); }
  } lists_frame(draw_lists_);
  const uint64_t cam_stamp = HashBytes(frame.camera.view.m, sizeof frame.camera.view.m, HashBytes(eye, sizeof(float) * 3, HashBytes(vp, sizeof vp, frame.viewer_space ? 1u : 2u)));   // S111 VIEWROWS review: View reads the view itself (CharCtx::view)
  const uint64_t time_stamp = HashBytes(&frame.time_s, sizeof(double));
  std::unordered_map<const assets::LightSet*, uint64_t> light_stamps;  // hashed once per set per frame
  static_assert(sizeof(assets::LightSet) == 244, "assets::LightSet changed: add the new field to light_stamp below before updating this size");   // S103 RL F4
  const auto light_stamp = [&](const assets::LightSet* l) -> uint64_t {
    const auto ins = light_stamps.try_emplace(l, 0);
    if (ins.second) {
      uint64_t h = HashBytes(&l->id, sizeof l->id, HashBytes(&l->edit_id, sizeof l->edit_id));
      h = HashBytes(l->dir, sizeof l->dir, h), h = HashBytes(l->col, sizeof l->col, h), h = HashBytes(l->sky, sizeof l->sky, h), h = HashBytes(l->ground, sizeof l->ground, h);
      h = HashBytes(l->wt, sizeof l->wt, h), h = HashBytes(l->hl_dir, sizeof l->hl_dir, h), h = HashBytes(l->hl_col, sizeof l->hl_col, h);
      h = HashBytes(&l->has_pcol, sizeof l->has_pcol, h), h = HashBytes(l->pcol, sizeof l->pcol, h), h = HashBytes(l->fall, sizeof l->fall, h), h = HashBytes(l->follow, sizeof l->follow, h), h = HashBytes(&l->follow_view, sizeof l->follow_view, h);   // S103 RL F4: follow_view changes the bound DifLghVec (a new LightSet field must join this hash: the size is pinned below)
      light_stamps[l] = h;
      return h;
    }
    return ins.first->second;
  };

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
    uint8_t cutout = 0;            // S103 F: Material::cutout_blend
    uint8_t pass = 0;              // hair / translucent: 1 = back faces culled, 2 = front faces culled (the viewer draws such a mesh twice)
    uint16_t inst = 0;             // S103 G: the placement instance (the kept draw lists' key)
    uint8_t zw = 0;                // S105 SZW: Material::stage_zw (bit 1 `s` / `a` slot, 2 `z`, 4 blended screen piece), always set (the census), applied only with wwe2k14_rdr_stage_zwrite
    int32_t order = -1;            // S105 SZW: the game's list position inside its `s` / `a` group (see_through_order.h SeeKey::order); -1 = none
    bool mirrored = false;         // SCU: a copy of a placement whose 3x3 determinant is negative = the arena's `r` copy (the only such placement our loader makes, YCP / CUL_ADV D); never `[0] < 0` (true for a turn too)
    bool piece_cut = false;        // S108 HAIR: ModelData::Mesh::piece_cut (not under the old-rule plant): GEQUAL 128/255 alpha test + depth write, still blended and drawn twice
    int32_t place = -1;            // S111 PROPORDER (draw_place_rows.h StampItems): the running category-33 place of a character-rank item (Game order only); -1 = none
    bool place_block = false;      // S111 PROPORDER: the object is an entrance prop pass a CWeapon category-33 node draws (its draws, alpha or additive, take the character rank)
    bool late_body = false;        // S111 CATORDER (draw_place_rows.h TakeLateBodies): an opaque mesh of a body the game draws after see-through categories (42), moved into the blended list at its body place
    bool late_node = false;        // S111 PODGLASS review (splat_state.h LateNodeOf): the object's node is of a category after 33 (the chamber's 35 / 36): its alpha draws come after the character rank
    int32_t char_slot = -1;        // S111 SEECHAR (piece_owner.h CharSlotOf): the character slot the item's object holds (a body "actor:slot:N", a piece linked to it); -1 = none. The opaque Body turn and the see-through character rank read it
  };
  struct ObjInfo {                 // per drawn object: its palette (skeleton node -> viewer-space matrix, world included) and light set
    assets::Matrix world;
    const Mat4* skin = nullptr;
    uint32_t skin_count = 0;
    const assets::LightSet* light = nullptr;
    uint32_t id = 0, asset = 0;    // S103 G: the record's object id and the model (the kept draw lists' key)
    uint32_t frame_object = 0;     // S103 SWF: the index in frame.objects (NOT in objs: invisible objects are skipped) that SceneFrame::object_engine / piece_alphas name
    uint64_t name_hash = 0;        // S103 P7 (review F1): hash of the object id TEXT (SceneTables::Str), only when the own blend tags are on; Object::id is a blob offset and is never a blend key
    const float* amb_rgb = nullptr;   // S111 CAARING: Object::tint of a kObjAmbientColour object (its rgb replaces MatAmbCol's), else null
  };
  std::vector<ObjInfo> objs;
  const SceneTables* const blend_names = fb.BlendTagsOn() ? frame.names : nullptr;   // S103 P7 (review F1): the own blend key hashes an object's name text; no record tables = no own tags
  objs.reserve(frame.objects.size());
  SeeOrder see_order = frame.see_order;  // S103 F: the see-through rule of this frame (see_through_order.h); read before the items are made
  SeeOrderFromEnv(see_order);
  std::vector<Item> opaque, blended;
  // S101 lane 15 (entrances): the light sets of a model as the entrance rig rewrote them this frame (the last entry = the default rig)
  std::vector<const ModelLights*> model_sets(models_.size(), nullptr);
  for (const ModelLights& ml : frame.model_lights)
    if (ml.asset < models_.size()) model_sets[ml.asset] = &ml;
  const int hide_slot = g_hide_stage_slot.load(std::memory_order_relaxed);   // S103 CAWB dev plant (g_hide_stage_slot, front_end.h): -1 = no effect; 3 hides CAS_moya so a shot shows the body behind it
  if (identity_nodes_frame_) {   // S103 CAWS (CAWG row 4): last frame's skin nodes outside the palette (drawn with an identity matrix): the first 8 frames that had any, then one a 600
    identity_nodes_total_ += identity_nodes_frame_;
    if (identity_lines_ < 8 || (++identity_lines_ % 600) == 0) rdr::Log("[scene][identity] skin nodes >= the palette size drawn with an identity matrix: %llu in the last frame, %llu in all", (unsigned long long)identity_nodes_frame_, (unsigned long long)identity_nodes_total_);
    if (identity_lines_ < 8) ++identity_lines_;
    identity_nodes_frame_ = 0;
  }
  piece_stats_ = PieceMeshStats();   // S103 EYF
  unsigned hidden_meshes = 0;   // S103 CAWS: how many mesh copies the plant removed this frame (the [scene][hide] line says whether it matched anything)
  bool crowd_hook = false;  // S103 lane D: a crowd stage hook draws every Crowd-class object of the record itself
  for (const StageHook* h : frame.stage_hooks) crowd_hook = crowd_hook || (h && std::strcmp(h->Name(), "crowd") == 0);
  // SCU: the stage record's cull (wwe2k14_rdr_stage_cull, per frame through the switch table; before the first refresh / in a tool: g_stage_cull, the rig's env) and its test-only plant
  const bool stage_cull_on = rdr_sw::BoolSeen(rdr_sw::kStageCull, g_stage_cull.load(std::memory_order_relaxed), frame.names ? frame.names->header.guest_frame : 0);   // (Seen: the "[switch] seen" line an in-run A/B verdict cuts at)
  const bool plant_no_flip = g_plant_stage_cull_no_flip.load(std::memory_order_relaxed);
  // S105 C3: wwe2k14_rdr_family_cull (every other model family's record cull; the character two-pass keys on the bit) and its test-only plant (the winding swapped on those families)
  const bool family_cull_on = rdr_sw::BoolSeen(rdr_sw::kFamilyCull, g_family_cull.load(std::memory_order_relaxed), frame.names ? frame.names->header.guest_frame : 0);
  const bool plant_family_flip = g_plant_family_cull_flip.load(std::memory_order_relaxed);
  // S105 SZW: wwe2k14_rdr_stage_zwrite (per frame through the switch table; before its first refresh / in a tool: g_stage_zwrite, the rig's env) and its test-only plant (a depth write with no forced alpha test)
  const bool zw_on = rdr_sw::BoolSeen(rdr_sw::kStageZwrite, g_stage_zwrite.load(std::memory_order_relaxed), frame.names ? frame.names->header.guest_frame : 0);
  const bool plant_zw_no_test = g_plant_szw_no_test.load(std::memory_order_relaxed);
  szw_ = SzwCensus{};
  const bool plant_pc_no_test = g_plant_piece_cut_no_test.load(std::memory_order_relaxed);   // S108 HAIR planted twin (TEST ONLY)
  const bool plant_pc_no_twin = g_plant_piece_cut_no_twin.load(std::memory_order_relaxed);   // S108 HAIR review (TEST ONLY): drives the no-twin rule
  hc_ = HairCutCensus{};
  std::set<std::pair<const void*, bool>> d2_seen;   // S103 WGD (dev, WWE_RDR_FEATURE_DUMP): the (mesh, mirrored copy) pairs this Record already wrote a "d2" line for (SCU: an `r` mesh writes its base and its mirrored copy; empty without the dump)
  for (const Object& obj : frame.objects) {
    const uint32_t obj_no = uint32_t(&obj - frame.objects.data());   // S103 EYF: the index SceneFrame::piece_alphas names
    if (!(obj.flags & kObjVisible) && !(obj.flags & kObjPiecesOnly)) continue;   // S111 PIECEROUTE: a pieces-only owner is drawn here (its piece meshes, below) and nowhere else
    if (crowd_hook && obj.klass == rdr_adapter::DrawClass::Crowd) continue;  // S103 lane D
    if (obj.asset >= models_.size() || !models_[obj.asset]->live || models_[obj.asset]->hook_owned) continue;  // (hook_owned: S103 lane D)
    ModelData& m = *models_[obj.asset];
    assets::Matrix ow;
    std::memcpy(ow.data(), obj.world.m, sizeof(float) * 16);
    ObjInfo oi;
    oi.world = ow;
    oi.id = obj.id, oi.asset = obj.asset, oi.frame_object = obj_no;  // S103 G; S103 SWF
    if (blend_names) oi.name_hash = g_own_blend_plant.load(std::memory_order_relaxed) == 1 ? uint64_t(obj.id) + 1 : OwnNameHash(blend_names->Str(obj.id));   // (plant 1: the raw blob offset, the review F1 bug)
    if ((obj.flags & kObjSkinned) && obj.bones_first + size_t(obj.bones_count) <= frame.bones.size() && obj.bones_count)
      oi.skin = &frame.bones[obj.bones_first], oi.skin_count = obj.bones_count;
    if (obj.light_set < frame.light_sets.size()) oi.light = &frame.light_sets[obj.light_set];
    if (obj.flags & kObjAmbientColour) oi.amb_rgb = obj.tint;   // S111 CAARING
    objs.push_back(oi);
    const uint32_t object_index = uint32_t(objs.size() - 1);
    const int obj_char_slot = CharSlotOf(frame.names ? frame.names->Str(obj.id) : std::string_view(), obj.flags, obj.layer);   // S111 FADECAW review / SEECHAR (piece_owner.h): once per object, read by both orders below
    for (const ModelData::Mesh& me : m.meshes) {
      // S101 lane 13: which of the model's first 64 meshes are shown (attire pieces, the eyelid layer)
      if (me.src < 32 ? !((obj.mesh_mask_lo >> me.src) & 1u) : (me.src < 64 && !((obj.mesh_mask_hi >> (me.src - 32)) & 1u))) continue;
      if ((obj.flags & kObjPiecesOnly) && !PiecesOnlyMeshDrawn(me.answers_piece_row, me.src_key)) continue;   // S111 PIECEROUTE (piece_row_rule.h): the game drew this owner's pieces, not its body
      // S103 EYF: the game fades each own-skeleton piece with ITS model's alpha (VS c185.x = [model+252]; 0 in an entrance for the eyelid / lash layer 20002): the record's piece row for this owner + piece id.
      // A lid-layer mesh with no row (its piece has no object of its own: a palette borrowed from the body) is not drawn: the viewer's rule (view\renderer.cpp lid_layer), a stand-in [L].
      float piece_alpha = 1.f;
      if (me.answers_piece_row) {   // S105 A5b (MeshAnswersPieceRow, decided at AddModelImpl); S111 GUESTKEY: a guest body mesh's src_key is 0, a guest piece model's its piece id (the stock keys)
        const SceneFrame::PieceAlpha* row = FindPieceRow(frame.piece_alphas, obj_no, me.src_key);   // S111 SOCKSHADOW (piece_row_rule.h): the one lookup the colour and caster passes share (the first row)
        const bool have_row = row != nullptr;
        if (have_row) piece_alpha = row->alpha;
        if (!have_row && me.lid_layer) { ++piece_stats_.lid_no_row_skipped; continue; }
        if (have_row && (row->undrawn || !(piece_alpha > 0.f))) { ++piece_stats_.skipped_alpha0; continue; }   // alpha 0, or a piece the owner's mesh does not draw at all (hidden / detached, alpha 0 too): nothing of it shows
        if (have_row) ++piece_stats_.drawn;
      }
      if (!me.extra_pieces.empty() && !frame.absent_extras.empty()) {   // S103 W2B: a chunk that skins to the extra nodes of a piece the record could not supply: its rows are zeros, so it is not drawn (the rest of the body is)
        bool absent = false;
        for (const auto& ae : frame.absent_extras)
          if (ae.first == obj_no && std::find(me.extra_pieces.begin(), me.extra_pieces.end(), ae.second) != me.extra_pieces.end()) { absent = true; break; }
        if (absent) { rdr_scene::g_spring_census.mesh_skipped.fetch_add(1, std::memory_order_relaxed); continue; }
      }
      if (me.skinned && !me.chunk) {  // a skinned piece of a class that is not a character: its bone inputs are not wired (counted, not drawn)
        EffectCoverage& cov = coverage_[me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)"];
        cov.effect = me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)";
        if (cov.reason.empty()) cov.reason = "skinned piece of a class that is not a yCh_* effect: skinning is wired for the characters' effects only";
        ++cov.meshes_unbound;
        if (FILE* f = fdump::File(); f && !in_seed_) {   // S103 WGD (dev): the "d2" line of a chunk that is never drawn (tools\harvest\choice_ours.py; the drawn ones are written in emit below; never the seed frame's Record)
          const bool ph1 = frame.phase_word >= 0 ? frame.phase_word == 1 : !frame.entrance;   // (the same rule as phase1 below)
          if (d2_seen.insert({&me, false}).second) {
            const std::string js = "{\"k\":\"d2\",\"part\":\"" + fdump::Esc(me.part) + "\",\"slot\":" + std::to_string(me.slot_no) + ",\"mesh\":" + std::to_string(me.src_record) + ",\"key\":" + std::to_string(me.src_key) +
                                   ",\"src_pac\":\"" + fdump::Esc(me.src_pac) + "\",\"arena\":" + std::to_string(m.arena_id) + ",\"shader\":\"" + fdump::Esc(cov.effect) + "\",\"vs\":\"\",\"ps\":\"\",\"path\":\"skinned_unwired\",\"phase1\":" + (ph1 ? "1" : "0") +
                                   ",\"ent\":" + (frame.entrance ? "1" : "0") + ",\"grp\":" + std::to_string(int(m.group)) + "}\n";
            std::fwrite(js.data(), 1, js.size(), f);
            std::fflush(f);
          }
        }
        continue;
      }
      const assets::Material* mt = me.material >= 0 ? &m.materials[size_t(me.material)] : nullptr;
      const bool sorted = mt && mt->alpha_blend;
      const size_t instances = me.placement >= 0 ? m.placements[size_t(me.placement)].size() : 1;
      if (sorted) {   // S103 CAWS (CAWG row 12): one capped line per blended mesh, so the white panels are NAMED (part, stage slot key, first node name, effect, picture, blend)
        static std::mutex bmu;
        static std::set<std::string> bseen;   // S105 A5b F3: keyed on (model label, part, shader), not the mesh pointer: an editor slider drag rebuilds the model and used the 24 lines up
        static int blines = 0;
        std::lock_guard<std::mutex> lk(bmu);
        if (blines < 24 && bseen.insert(m.label + "|" + me.part + "|" + mt->shader).second) {
          ++blines;
          const int di = mt->diffuse;
          const bool have_pic = di >= 0 && size_t(di) < m.img_info.size() && size_t(di) < m.images.size();
          rdr::Log("[scene][blended] model=%s part=%s slot_no=%d node=%s src=%s:%d shader=%s blend=%d (1 alpha, 2 additive) picture=%d %s%ux%u mips=%u fmt=%d image=%u instances=%zu",
                   m.label.c_str(), me.part.c_str(), me.slot_no, me.node.c_str(), me.src_pac.c_str(), me.src_key, mt->shader.c_str(), int(mt->additive ? 2 : 1), di, have_pic ? "" : "(none) ",
                   have_pic ? m.img_info[size_t(di)].w : 0u, have_pic ? m.img_info[size_t(di)].h : 0u, have_pic ? m.img_info[size_t(di)].mips : 0u, have_pic ? m.img_info[size_t(di)].fmt : -1, have_pic ? unsigned(m.images[size_t(di)]) : 0u, instances);
        }
      }
      const uint8_t cull = (me.slot_no >= 0 && size_t(me.slot_no) < m.stage_cull.size()) ? m.stage_cull[size_t(me.slot_no)] : uint8_t(0);   // S103 V1
      for (size_t k = 0; k < instances; ++k) {
        if (cull && ((me.placement >= 0 && m.placements[size_t(me.placement)][k][0] < 0.f) ? (cull & 2) : (cull & 1))) continue;   // S103 V1: a rejected copy (the mirror's x scale is negative); k keeps its number
        if (hide_slot >= 0 && me.slot_no == hide_slot) { ++hidden_meshes; continue; }
        Item it;
        it.model = &m;
        it.mesh = &me;
        it.world = me.placement >= 0 ? assets::Mul(m.placements[size_t(me.placement)][k], ow) : ow;
        if (me.placement >= 0) {   // SCU: the placement's own determinant (not it.world's: a non-arena object with a mirrored world is drawn by the game with no winding swap, CUL_ADV D correction 1)
          const float* p = m.placements[size_t(me.placement)][k].data();
          it.mirrored = p[0] * (p[5] * p[10] - p[6] * p[9]) - p[1] * (p[4] * p[10] - p[6] * p[8]) + p[2] * (p[4] * p[9] - p[5] * p[8]) < 0.f;
        }
        const bool force_blend = (obj.flags & kObjForceBlend) != 0 && !sorted && mt;   // S103 CGF: an opaque mesh of a slot-32 stage structure is blended by the object's alpha (MdlAlpha); depth write stays the material's (opaque: on)
        const uint8_t node_blend = mt ? splat_state::NodeBlendOf(obj.flags) : uint8_t(0);   // S111 PROPORDER review (splat_state.h): a mesh a CWeapon category-33 node draws is blended by the NODE's code (1 alpha, 2 additive) whatever its material; its z write off rides on kObjNoDepthWrite
        it.blend = !mt ? 0 : node_blend ? node_blend : sorted ? (mt->additive ? 2 : 1) : (mt->blend_inline || force_blend) ? 1 : 0;
        it.depth_write = splat_state::DepthWriteOf(obj.flags, sorted, see_order == SeeOrder::Game && mt && mt->cutout_blend == 1);  // S103 F: a prop cut-out drawn as blended keeps its depth write in Game order; S108 BLD2: the object's own z-write-off (floor splats)
        // S108 HAIR (piece_draw_rule.h): the game draws a stock piece's translucent mesh (hair 10000 ...) alpha-tested GEQUAL 128/255, blended AND writing depth (sub_8259E488 +0x74 / +0x1B4); emit binds the alpha twin, and a pair
        // with no twin keeps depth write off (a draw_shortfall). The planted twin g_plant_piece_cut_old (TEST ONLY) is the draw before S108: blended, untested, no depth write.
        // S111 GUESTKEY: a guest-built piece model (a created superstar's hair 10000, accessories 10030-10033) carries its piece id in src_key like a stock piece mesh (guest_model_draw.h BuildModel), so
        // me.piece_cut (PieceCut by src_key, decided at AddModelImpl) is the one rule for both; the S108 plant turns it off.
        it.piece_cut = me.piece_cut && !g_plant_piece_cut_old.load(std::memory_order_relaxed);
        if (it.piece_cut) it.depth_write = true;
        it.zw = mt ? mt->stage_zw : uint8_t(0);
        bool zw_blended = false;
        if (zw_on && (it.zw & 3) && !force_blend && !node_blend) {   // S105 SZW (SZW_contract.md rule 3: depth write, alpha test and order land together): an `s` / `a` slot writes depth unless `z`; a `z`-only slot is blended, drawn in the opaque list, with no depth write
          if (it.zw & 1) { if (sorted) it.depth_write = !(it.zw & 2); }
          else it.blend = it.blend ? it.blend : uint8_t(1), it.depth_write = false, zw_blended = true;   // z only: into the BLENDED list at rank 0 (ahead of the `s` items): the game's bodies (categories 13-21) draw before group 0 (22), so a wrestler behind such a piece must not paint over it (review B2 item 6); the cutout_blend == 1 edge too
        }
        it.cutout = (mt && !node_blend) ? mt->cutout_blend : 0;   // (a node's pass is the game's own blend, no loader cut-out: it sorts by its category-33 place)
        it.sorted = sorted || force_blend || zw_blended || node_blend != 0;
        it.late_node = node_blend != 0 && splat_state::LateNodeOf(obj.flags);
        it.alpha = obj.tint[3] * piece_alpha;   // S103 EYF
        if (!splat_state::ApplyOwnNode(obj.flags, it.alpha, mt != nullptr, it.blend)) continue;   // S111 OWNNODEZ (+ review): an own-node / CWDA model below its node's reference is dropped whole (GEQUAL 1/255 / 128/255); else every mesh is blended by code 0, in its material's list with its depth write
        it.light_override = -1;
        it.ml = model_sets[obj.asset];
        it.object = object_index;
        it.char_slot = obj_char_slot;
        it.pass = 0;
        it.inst = uint16_t(k + ((obj.flags & kObjDrawnByOwner) ? 0u : uint32_t(obj.pad)));  // S103 G; S103 RGP: Object::pad is the ring's instance + 1 (the GPU-skinned ring: the instances of one key share the model and id 0, each needs its own kept draw); 0 for every other object, so k alone (a drawn-by-owner object carries a piece id there)
        const float cx = me.center[0] * it.world[0] + me.center[1] * it.world[4] + me.center[2] * it.world[8] + it.world[12] - eye[0];
        const float cy = me.center[0] * it.world[1] + me.center[1] * it.world[5] + me.center[2] * it.world[9] + it.world[13] - eye[1];
        const float cz = me.center[0] * it.world[2] + me.center[1] * it.world[6] + me.center[2] * it.world[10] + it.world[14] - eye[2];
        it.dist2 = cx * cx + cy * cy + cz * cz;
        // S105 C3 (C3.md, sub_82C04DF0 0x82C0A8BC-0x82C0A8DC): the game's t = 1 rule is "a translucent character mesh whose record bit 0x2 is CLEAR is drawn twice (front, back)"; a bit-set one is one cull-back draw. Under
        // wwe2k14_rdr_family_cull off: the hair effect names, whatever the bit (the draw before). U1 (which meshes the translucent node draws) is read since S111 SEECHAR (a created body's layers 5..7 and
        // every character's SUBM pieces: the class, see_through_order.h SeeCharacter), but this test stays on the effect: the viewer's own frame (renderer_rdr.cpp ShotPixels) names no object, so
        // Item::char_slot is -1 for its wrestlers and a class key would drop their hair's back pass (docs\research\s111\SEECHAR.md, Review). A shader-side use: tools\lint\char_class_key_lint.py allows it.
        const bool two_pass = mt && IsCharEffect(mt->shader) && family_cull::CharTwoPass(sorted, IsHairEffect(mt->shader), mt->cull_word, family_cull_on);
        if (two_pass) {  // the game draws such a mesh twice (renderer.cpp BuildSceneList)
          it.pass = 1;
          blended.push_back(it);
          it.pass = 2;
          blended.push_back(it);
        } else {
          (it.sorted ? blended : opaque).push_back(it);
        }
      }
    }
  }
  if (hide_slot >= 0) {   // S103 CAWS: the plant's own answer (CAWG row 11): did the hide-stage-slot plant match any mesh? (a line on the first frame and whenever the count changes, at most 6)
    static unsigned last_hidden = ~0u;
    static int hlines = 0;
    if (hidden_meshes != last_hidden && hlines < 6) ++hlines, last_hidden = hidden_meshes, rdr::Log("[scene][hide] hide_stage_slot=%d hid %u mesh copies this frame%s", hide_slot, hidden_meshes, hidden_meshes ? "" : " (NO mesh has that slot_no)");
  }
  // S111 FADEORDER (body_order.h): the opaque list in the game's category turns around the bodies (13 + slot): the category-3 content (a cycled arena slot, a chamber category-3 row), then every
  // character's own object by character slot (the record's wrestler order is the category-33 list's, not the slot's), then the rest (22 on: ring, arena, screens, props, event props), each in record
  // order. A wrestler colour draw blends (ALPHAWREST), so a faded body shows what was drawn before it. S111 FADECAW: the Body turn is the object's class (a slot body, or a piece linked to it: what the
  // character's opaque node sub_825A3908 draws), never the mesh's effect, so a created superstar's yDefault part draws in its body's turn.
  body_order::OrderOpaque(opaque, [&](const Item& i) {
    const uint32_t fo = i.object < objs.size() ? objs[i.object].frame_object : ~0u;
    const Object* fob = fo < frame.objects.size() ? &frame.objects[fo] : nullptr;
    const bool marked = fob && (fob->flags & kObjBeforeBodies) != 0;
    const bool cycled = i.model->arena_id >= 0 && body_order::CycledSlot(i.model->slot_records, i.mesh->slot_no);
    const int slot = i.char_slot;   // piece_owner.h CharSlotOf of the item's object (set once per object above): a body "actor:slot:N", else a linked piece's owner (Object::layer = slot + 1); -1 = none
    const bool char_effect = body_order::g_plant_effect_body && i.mesh->material >= 0 && IsCharEffect(i.model->materials[size_t(i.mesh->material)].shader);   // (read only by the unit's plant)
    const body_order::Turn turn = body_order::TurnOf(body_order::BodyObject(slot, char_effect), marked || cycled);
    return body_order::PlaceOf(turn, turn == body_order::Turn::Body ? slot : -1);
  });
  {  // S103 F: the see-through order (see_through_order.h): the frame's rule, or WWE_RDR_ORDER for the offline tools; Distance = the line this replaced
    const bool game_order = see_order == SeeOrder::Game;
    if (zw_on && game_order) {   // S105 SZW (ZW1 [C code + capture]): the game's `s` group (and `a` group) list = every slot ascending, drawn in runs by light set, the set seen first first (sub_825B9F10 + sub_825B9970); inside a slot the record order stays
      for (const uint8_t group_blend : {uint8_t(1), uint8_t(2)}) {
        std::vector<size_t> idx;
        for (size_t bi = 0; bi < blended.size(); ++bi)
          if (blended[bi].cutout == 2 && blended[bi].blend == group_blend) idx.push_back(bi);
        std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return blended[x].mesh->slot_no < blended[y].mesh->slot_no; });
        std::vector<std::pair<int, int>> run_of;   // light-set id -> run number, first seen first
        for (const size_t bi : idx) {
          Item& bit = blended[bi];
          const int ls = bit.mesh->light_set >= 0 && size_t(bit.mesh->light_set) < bit.model->light_sets.size() ? bit.model->light_sets[size_t(bit.mesh->light_set)].id : -1000000;
          int run = -1;
          for (const auto& pr : run_of) if (pr.first == ls) { run = pr.second; break; }
          if (run < 0) run = int(run_of.size()), run_of.push_back({ls, run});
          bit.order = run * 1024 + std::max(0, std::min(bit.mesh->slot_no, 1023));
          ++szw_.order_items;
        }
      }
    }
    if (game_order) {   // S111 PROPORDER (draw_place_rows.h StampItems): each item's object's place in the game's category-33 list (SceneFrame::draw_places); the character rank takes the running place
      // S111 CATORDER (draw_place_rows.h TakeLateBodies): a body whose opaque node draws after see-through categories (42 in a re-keyed entrance) leaves the opaque list for its body place
      draw_place::TakeLateBodies(frame.draw_places, frame.objects.size(), opaque, blended, [&objs](const Item& i) { return i.object < objs.size() ? objs[i.object].frame_object : ~0u; });
      draw_place::StampItems(frame.draw_places, frame.objects.size(), blended, [&objs](const Item& i) { return i.object < objs.size() ? objs[i.object].frame_object : ~0u; }, [](const Item& i) {
        SeeKey k;
        k.blend = i.blend, k.place_block = i.place_block, k.late_node = i.late_node, k.place = i.place;   // S111 REKEYORDER2: StampItems sets the place first (SeeRank's re-keyed rank reads it)
        k.character = SeeCharacter(i.char_slot, g_plant_see_effect.load(std::memory_order_relaxed) && i.mesh->material >= 0 && IsCharEffect(i.model->materials[size_t(i.mesh->material)].shader));   // S111 SEECHAR: the object's class (the effect only for the unit's plant)
        return SeeRank(k);
      });
    }
    SortSeeThrough(see_order, blended, [game_order](const Item& i) {
      SeeKey k;
      k.blend = i.blend, k.dist2 = i.dist2, k.cutout = i.cutout, k.order = i.order, k.place = i.place, k.place_block = i.place_block, k.late_node = i.late_node;
      if (game_order) k.character = SeeCharacter(i.char_slot, g_plant_see_effect.load(std::memory_order_relaxed) && i.mesh->material >= 0 && IsCharEffect(i.model->materials[size_t(i.mesh->material)].shader));  // (only Game mode reads it: Distance = today) S111 SEECHAR: the object's class
      return k;
    });
  }
  // S103 CAWB (release, once per 600 frames, set by rf_wrestlers): the body probe line = where the guest-model body is and how many additive items sit nearer the eye than it (CAWV Q1: the CAS_moya cards hide the body).
  if (frame.body_probe.valid) {
    const auto& bp = frame.body_probe;
    const float d2 = bp.dist * bp.dist;
    unsigned add_nearer = 0, add_all = 0;
    for (const Item& bi : blended) if (bi.blend == 2) ++add_all, add_nearer += bi.dist2 < d2 ? 1u : 0u;
    rdr::Log("[rdr][caw] body at (%.1f %.1f %.1f) eye-dist %.1f view-z %.1f ndc (%.3f %.3f) bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f) additive_nearer %u of %u (frame %llu)", bp.pos[0], bp.pos[1], bp.pos[2], bp.dist, bp.view_z, bp.ndc[0], bp.ndc[1],
                 bp.bmin[0], bp.bmin[1], bp.bmin[2], bp.bmax[0], bp.bmax[1], bp.bmax[2], add_nearer, add_all, (unsigned long long)bp.guest_frame);
  }
  // S103 F: WWE_RDR_ORDER_LOG=1 prints the order the colour pass will draw the see-through list in (dev: docs\renderer\SEE_THROUGH_ORDER.md)
  if (static const bool order_log = std::getenv("WWE_RDR_ORDER_LOG") != nullptr; order_log) {
    for (const Item& it : blended) {
      const assets::Material* mt = it.mesh->material >= 0 ? &it.model->materials[size_t(it.mesh->material)] : nullptr;
      std::fprintf(stderr, "[order] blended %-18s part=%-10s blend=%d dw=%d pass=%d dist=%.1f\n", mt ? mt->shader.c_str() : "(none)", it.mesh->part.c_str(), int(it.blend), int(it.depth_write), int(it.pass), std::sqrt(it.dist2));
    }
  }

  // S103 lane S: the classes' stage hooks (stage_hook.h)
  StageCtx sctx;
  sctx.frame = &frame, sctx.view_proj = vp, sctx.eye = eye, sctx.color = tg.color, sctx.depth = tg.depth;
  sctx.width = tg.width, sctx.height = tg.height, sctx.renderer = &renderer_;
  const auto run_stage = [&](Stage s) { for (StageHook* h : frame.stage_hooks) if (h) h->Record(s, sctx, fb); };
  run_stage(Stage::Maps);
  rdr::TargetsOp targets;
  targets.color[0].image = tg.color;
  targets.color_count = 1;
  targets.depth.image = tg.depth;
  fb.SetTargets(targets);
  rdr::ClearOp clear;
  clear.flags = rdr::kClearColor | rdr::kClearDepth | rdr::kClearStencil;  // (skipped when !tg.clear)
  clear.full = 1;
  for (int i = 0; i < 4; ++i) clear.color[i] = frame.clear[i];
  clear.depth = 1.0f;
  if (tg.clear) fb.Clear(clear);
  run_stage(Stage::BeforeOpaque);

  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(tg.width);
  dynamic.viewport[3] = float(tg.height);
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = tg.width;
  dynamic.scissor.height = tg.height;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;  // clip y is up (D3D-style matrices); the viewport is Vulkan's
  dynamic.system[9] = 7.f;
  // S103 B2: the alpha test's reference / function come from the draw's PASS CLASS (pass_state.h, the game's own literals: lit / arena / hair GEQUAL 1/255, crowd 192/255, see-through 128/255), not one 0.5
  const auto alpha_dyn = [&](pass_state::PassClass c) { return AlphaDynamic(dynamic, c, options_.plant_old_alpha_ref); };

  const assets::LightSet default_rig = assets::DefaultRig();
  // S103 L6: the wrestlers' shadow map of this frame (lane E: made at Stage::Maps; raw today), or none: then every character draw takes its plain variant as before
  const MapImage* shadow_map = nullptr;
  static const bool no_shadow_reader = std::getenv("WWE_RDR_NO_SHADOW_READER") != nullptr;   // S103 L6 (dev / the rig's planted twin of the old behaviour): the plain variants as before
  if (frame.maps && !no_shadow_reader) {
    const MapImage* sm = frame.maps->Find(MapRole::ShadowMap);
    // S103 L6: a RAW map is accepted on purpose: lane E's map has no blur / min-pair chain yet (hard edges expected), and shading from it is closer to the game than none. Counted: shadow_raw_bound.
    if (sm && sm->image != rdr::kNoImage) shadow_map = sm;
  }
  const auto standin = [&](int i, MapRole role) -> const MapImage* {   // S106 PSEED (tool only): a present-map arm of the pipeline census
    MapImage& m = census_maps_[i];
    m.role = role, m.image = white_, m.raw = false;
    return white_ != rdr::kNoImage ? &m : nullptr;
  };
  if (!shadow_map && (options_.census_maps & 1u)) shadow_map = standin(0, MapRole::ShadowMap);   // S106 PSEED
  // S103 PMS: the map the shadowed FLOOR reads at slot 12 (yBG_* FX_SHADOW): the game's PROPS map (the circle blobs, live_map_props.cpp), never the wrestlers' map B (that was the S98 'long stretched shadows on the floor').
  // (S103 RML: the same switch gates the ring mat's light pass; the receivers' vertex ShadowPrm (0, 0, 0, 1) x 2 is the game's value and is fed on or off.)
  // No props map this frame = no floor shadow term (the pass without +2, counted d1_shadow_nomap). Switch --wwe2k14_rdr_props_map off = the old binding: the floor reads map B with the wrestlers' light.
  const bool props_on = props_map::On();
  const MapImage* props_img = nullptr;
  if (frame.maps && props_on) {
    const MapImage* pm = frame.maps->Find(MapRole::PropsMap);
    if (pm && pm->image != rdr::kNoImage) props_img = pm;
  }
  if (!props_img && props_on && (options_.census_maps & 2u)) props_img = standin(1, MapRole::PropsMap);   // S106 PSEED (tool only)
  if (props_img && props_nudge_ != 0.f) { props_nudged_ = *props_img; props_nudged_.matrix[0] += props_nudge_; props_img = &props_nudged_; }   // S103 RML (rig only): the map moves between frames
  if (props_img) ++pms_site_[0];   // site 0: the reader finds the props map
  const MapImage* floor_map = props_on ? props_img : shadow_map;
  // S103 L9: the ring mat's plane shadow (lane E's chain made it: a mask, a box downscale, two blurs; never raw), or none: the mat keeps its plain variant
  const MapImage* plane_map = nullptr;
  float plane_m[16] = {};
  static const bool no_plane_reader = std::getenv("WWE_RDR_NO_PLANE_READER") != nullptr;   // S103 L9 (the planted twin of the old behaviour)
  if (frame.maps && !no_plane_reader) {
    const MapImage* pm = frame.maps->Find(MapRole::PlaneShadow);
    if (pm && pm->image != rdr::kNoImage && !pm->raw) {
      plane_map = pm;
      std::memcpy(plane_m, pm->matrix, sizeof plane_m);
      static const bool plant_noflip = std::getenv("WWE_RDR_PLANT_PLANE_NOFLIP") != nullptr;   // S103 L9 dev plant: the lookup is not taken into the viewer's y (a mirrored lookup)
      if (frame.viewer_space && !plant_noflip)  // the viewer's space is the game's with y negated (the mask draws the same way: live_map_plane.cpp): the matrix takes the viewer's y
        for (int r = 0; r < 4; ++r) plane_m[4 * r + 1] = -plane_m[4 * r + 1];
    }
  }
  if (!plane_map && (options_.census_maps & 4u)) plane_map = standin(2, MapRole::PlaneShadow);   // S106 PSEED (tool only; plane_m stays the identity-free zero it starts as: the census reads no pixels)
  // S103 L7: the frame's FILTERED rim map (lane E2's pass + the game's blur, live_map_chain.cpp). Only a map that went through the chain is bound (a raw one stays the black stand-in: the shader's rim reads it
  // as a coverage + depth fade and a raw map has hard edges); WWE_RDR_RAW_RIM_BOUND=1 (dev plant) binds the raw map anyway.
  const MapImage* rim_map = nullptr;
  if (frame.maps) {
    static const bool raw_rim_ok = std::getenv("WWE_RDR_RAW_RIM_BOUND") != nullptr;
    const MapImage* rm = frame.maps->Find(MapRole::RimMap);
    if (rm && rm->image != rdr::kNoImage && (!rm->raw || raw_rim_ok)) rim_map = rm;
  }
  // S103 SWF: the liquid map of the wrestler whose body object is `obj` (frame.objects index): MapRole::SweatMap, params[0] = the BIT PATTERN of the body's engine object (SceneFrame::object_engine, from the record's kScEngineObjects;
  // a guest address does not survive a float compare). No engine object for the body, or no map for it: null (the draw stays dry).
  const auto liquid_of = [&](size_t obj) -> const MapImage* {
    if (!frame.maps) return nullptr;
    const uint32_t key = EngineObjectOf(frame.object_engine, uint32_t(obj));   // obj = the FRAME index (liquid_join.h)
    if (!key) return nullptr;
    for (const MapImage& mi : frame.maps->maps) {
      if (mi.role != MapRole::SweatMap || mi.image == rdr::kNoImage) continue;
      uint32_t k = 0;
      std::memcpy(&k, &mi.params[0], 4);
      if (k == key) return &mi;
    }
    return nullptr;
  };
  // S103 D1: the read rule for a stage-slot mesh (read_rule.h; the same branches as tools\harvest\choice_predict.py, cited there by claim + instruction range).
  float shadow_bg_m[16] = {};
  if (floor_map) {   // S103 PMS: the floor's map (the props map; map B with the switch off)
    std::memcpy(shadow_bg_m, floor_map->matrix, sizeof shadow_bg_m);
    if (props_on) ++pms_site_[1];   // site 1: the floor's matrix comes from the props map
    if (frame.viewer_space) for (int r = 0; r < 4; ++r) shadow_bg_m[4 * r + 1] = -shadow_bg_m[4 * r + 1];   // the viewer's space is the game's with y negated (the plane map's rule above)
  }
  static const bool d1_old = std::getenv("WWE_RDR_D1_OLD") != nullptr;   // S103 D1 dev plant: the old heuristics (the twin)
  const std::shared_ptr<const read_rule::EffectTable> rule_table = d1_old ? nullptr : read_rule::EffectTableNow();
  const bool phase_known = frame.phase_word >= 0;
  const bool phase1 = phase_known ? frame.phase_word == 1 : !frame.entrance;   // [L] without the record's phase row: an entrance = phase 4, else 1 (stage_look's signal), counted
  // The pass's shaders -> our variant (fx_registry kHashes; an equal hash in several effects: the one of this effect, as effect_table.py FxNames.name prefers).
  const auto variant_of = [&](uint64_t h, const std::string& effect, bool vertex) -> int {
    const auto* first = std::begin(rdr::fx::kHashes);
    const auto* last = std::end(rdr::fx::kHashes);
    const auto* it = std::lower_bound(first, last, h, [](const rdr::fx::HashEntry& e, uint64_t v) { return e.hash < v; });
    int any = -1;
    const std::string prefix = effect + "_";
    for (; it != last && it->hash == h; ++it) {
      const char* n = kVariants[it->variant].name;
      if (!n || EndsWith(n, vertex ? "_vertex" : "_fragment") == false) continue;
      if (std::strncmp(n, prefix.c_str(), prefix.size()) == 0) return int(it->variant);
      if (any < 0) any = int(it->variant);
    }
    return any;
  };
  struct RuleOut { int array = 384, tech = -1, pass_rule = -1, pass_used = -1, vs = -1, ps = -1; bool shadow = false, shadow_used = false; std::string effect, why; };
  const auto rule_choose = [&](ModelData& m, const ModelData::Mesh& me, RuleOut& r) -> bool {
    if (!rule_table) return r.why = d1_old ? "WWE_RDR_D1_OLD" : "no effect table", false;
    const assets::Material& am = m.materials[size_t(me.material)];
    read_rule::SlotRec sr;
    {
      const auto sit = std::lower_bound(m.slot_records.begin(), m.slot_records.end(), me.slot_no, [](const std::pair<int, assets::Model::SlotRecord>& p, int s) { return p.first < s; });
      if (sit != m.slot_records.end() && sit->first == me.slot_no) sr.b6 = sit->second.b6, sr.b7 = sit->second.b7, sr.b8 = sit->second.b8, sr.b9 = sit->second.b9, sr.b10 = sit->second.b10, sr.cycled = sit->second.cycled;   // cycled: S103 UVF (sel_cycled)
    }
    const int group = read_rule::DrawGroup(sr);                         // [draw_group: R8]
    const int sel = read_rule::Selector(sr, group, phase1);             // [stage_sel / sel_group: R7, R10 class1] (the outer selector: default 0)
    const std::string key = read_rule::Lower(am.shader);                // [arr384: R1 rec[name]] the record +0x7C name, lowercased
    const auto e384 = rule_table->find(key);
    if (e384 == rule_table->end()) return r.why = "effect not in the table", false;
    const read_rule::EffectTech* eff = &e384->second;
    r.array = 384;
    if (sel != 0) {                                                     // [sel_388: R3 colour-pass selector] selector != 0 -> +388 = the 'LM' twin, else a copy of +384 [arr388_twin]
      r.array = 388;
      const auto e388 = rule_table->find(read_rule::TwinLower(key));
      if (e388 != rule_table->end()) eff = &e388->second;
    }
    r.effect = eff->name;
    const read_rule::RecFlags rf = read_rule::RecFlagsOf(key);          // [rec0 .. rec20: R1] the flags of the +384 effect's name
    const int m540 = read_rule::Model540(me.slot_no, m.arena_id);       // [skinned_slot: R10 class4]
    r.shadow = read_rule::ShadowSlot(sr, group, phase1);                // [shadow_p / shadow_gate: R10 class3, R11] (the gate: the predictor's default, passes)
    r.tech = int(am.technique);                                          // [PLAN.md skeleton R0] record word +0x90
    r.pass_rule = read_rule::PassIndex(rf, r.shadow, m540);             // [pass_base / p5ee0_*: R9]
    const bool skinned_mesh = me.skinned && me.chunk;
    const bool use_shadow = r.shadow && rf.rec4 && floor_map != nullptr;   // S103 PMS: the floor's map
    r.shadow_used = use_shadow;
    if (use_shadow && props_on) ++pms_site_[2];   // site 2: use_shadow is gated on the props map
    if (r.shadow && rf.rec4 && !floor_map) ++d1_shadow_nomap_;          // no shadow map this frame: the pass without +2 (counted; ShadowPrm 0 would be 0 / 0 in the receiver)
    const int m540_used = (m540 == 1 && !skinned_mesh) ? 0 : m540;
    if (m540_used != m540) ++d1_skin_standin_;                           // the game's skinned pass for a mesh ours keeps rigid: the unskinned twin (counted)
    r.pass_used = read_rule::PassIndex(rf, use_shadow, m540_used);
    if (r.tech < 0 || size_t(r.tech) >= eff->techs.size() || size_t(r.pass_used) >= eff->techs[size_t(r.tech)].size()) return ++d1_no_pass_, r.why = "technique / pass not in the effect", false;
    const read_rule::PassShaders& p = eff->techs[size_t(r.tech)][size_t(r.pass_used)];
    if (!p.vs_ok || !p.ps_ok) return ++d1_no_pass_, r.why = "pass shaders unread", false;
    r.vs = variant_of(p.vs, eff->name, true);
    r.ps = variant_of(p.ps, eff->name, false);
    if (r.vs < 0 || r.ps < 0) return r.why = "pass shader has no variant of ours", false;
    return true;
  };
  const auto RuleFor = [&](ModelData& m, const ModelData::Mesh& me) -> const Bound* {
    if (floor_map && props_on) ++pms_site_[3];   // site 3: the pick key's map bit is the floor's map
    const uint64_t pick_key = (uint64_t(&me - m.meshes.data()) << 2) | (uint64_t(phase1) << 1) | uint64_t(floor_map != nullptr);   // S103 PMS: the floor's map
    if (rule_table) if (auto pk = m.rule_pick.find(pick_key); pk != m.rule_pick.end()) { if (pk->second) ++d1_rule_; else ++d1_old_; return pk->second; }
    RuleOut r;
    const bool ok = rule_choose(m, me, r);
    const Bound* b = ok ? BindRule(m, me.material, me.skinned && me.chunk, r.vs, r.ps, r.shadow_used) : nullptr;
    if (ok && !b) r.why = "pair needs an input not fed";
    if (FILE* f = fdump::File()) {   // dev (WWE_RDR_FEATURE_DUMP): one line per mesh and look, for tools\scene\d1_check.py
      static std::set<std::tuple<const void*, int, bool>> seen;
      if (seen.insert({&me, int(phase1), d1_old}).second) {
        const std::string js = "{\"k\":\"d1\",\"part\":\"" + fdump::Esc(me.part) + "\",\"slot\":" + std::to_string(me.slot_no) + ",\"mesh\":" + std::to_string(me.src_record) + ",\"shader\":\"" +
                               fdump::Esc(m.materials[size_t(me.material)].shader) + "\",\"phase1\":" + (phase1 ? "1" : "0") + ",\"phase_known\":" + (phase_known ? "1" : "0") + ",\"array\":" + std::to_string(r.array) +
                               ",\"effect\":\"" + fdump::Esc(r.effect) + "\",\"tech\":" + std::to_string(r.tech) + ",\"pass\":" + std::to_string(r.pass_rule) + ",\"pass_used\":" + std::to_string(r.pass_used) +
                               ",\"shadow\":" + (r.shadow ? "1" : "0") + ",\"vs\":\"" + (r.vs >= 0 ? kVariants[r.vs].name : "") + "\",\"ps\":\"" + (r.ps >= 0 ? kVariants[r.ps].name : "") +
                               "\",\"rule\":" + (b ? "1" : "0") + ",\"why\":\"" + fdump::Esc(r.why) + "\"}\n";
        std::fwrite(js.data(), 1, js.size(), f);
      }
    }
    if (rule_table) m.rule_pick.emplace(pick_key, b);
    if (b) ++d1_rule_; else ++d1_old_;
    if (!phase_known) ++d1_phase_guess_;
    return b;
  };
  const float debug_colour[4] = {1.f, 0.f, 1.f, 1.f};
  std::vector<uint8_t> block;
  std::map<uint64_t, uint32_t> cloth_ring;   // S103 PVR: piece key -> its ring offset this frame
  std::set<uint64_t> cloth_done;  // the cloth pieces posed and uploaded this frame (a hair / translucent piece is drawn twice from one upload)
  uint32_t eng_obj = ~0u; int eng_mode = -1; const SceneFrame::ObjectEngine* eng_row = nullptr;   // S103 XLF: the CharModeOf / ObjectEngineRowOf scan once per object, not per mesh
  const auto emit = [&](const Item& it) {
    ModelData& m = *it.model;
    ApplyLmRow(m);   // S103 B3
    const ModelData::Mesh& me = *it.mesh;
    if (DebugEffectFilter(me.material >= 0 ? m.materials[size_t(me.material)].shader : std::string("(no material)"))) return;
    {  // S103 L (dev diagnosis): WWE_RDR_L_SLOTS=lo-hi draws only the arena parts "slot N" with lo <= N <= hi (a bisect of which stage slot draws what)
      static const char* range = std::getenv("WWE_RDR_L_SLOTS");
      int lo = 0, hi = 0, n = -1;
      if (range && std::sscanf(range, "%d-%d", &lo, &hi) == 2 && std::sscanf(me.part.c_str(), "slot %d", &n) == 1 && (n < lo || n > hi)) return;
    }
    if (static const std::string skip_tex = [] { const char* e = std::getenv("WWE_RDR_SKIP_TEX"); return std::string(e ? e : ""); }(); !skip_tex.empty() && me.material >= 0 &&
        skip_tex.find(m.materials[size_t(me.material)].diffuse_name) != std::string::npos && !m.materials[size_t(me.material)].diffuse_name.empty()) return;  // S101 lane 257 (dev): WWE_RDR_SKIP_TEX=name1,name2 leaves the meshes with those diffuse textures out  // S101 lane 257: WWE_RDR_SKIP_EFFECT / WWE_RDR_ONLY_EFFECT (dev)
    // S103 D1: a stage-slot mesh of an arena model takes the pair the READ RULE chooses (RuleFor above / read_rule.h) instead of the heuristics below; WWE_RDR_D1_OLD=1 (dev) = the old path (the twin).
    // Computed first: on a hit the old light-map mode and binds are skipped (review 5). Slots 0..398 are never the movie surfaces (800-809), so no part-name test here.
    const bool is_char = me.material >= 0 && IsCharEffect(m.materials[size_t(me.material)].shader);
    const Bound* rule_bound = (!is_char && m.arena_id >= 0 && me.slot_no >= 0 && me.slot_no <= 398 && me.material >= 0) ? RuleFor(m, me) : nullptr;
    if (rule_bound && rule_bound->plane && !plane_map) rule_bound = nullptr;   // (a plane reader with no plane map this frame: the old path, as L9)
    LmMode lm_mode = LmMode::Own;
    // S103 YCB (YCH.md section 4): a wrestler material takes the game's own (technique, pass) row (the pass: technique_pick.h CharPass: skinned model + part, the model's mode word, the shadow map, the spec-power record).
    // S111 OLDCHAIN: the table pick is the only pick. A successful one is char_b; every other answer (no table, no row, a blocked row) is char_hole: the draw is a counted HOLE (drawn nothing), never another variant.
    const Bound* char_b = nullptr;
    const Bound* char_hole = nullptr;   // S111 OLDCHAIN: the failed table bind (debug, no pair): Record's unbound exit draws nothing for it; counted char_hole_draws
    const float* char_cc = nullptr;   // S108 THR2: char_b is a change-colour bind: the body's 16 ChangeColor constants
    if (is_char) {
      const assets::Material& cm = m.materials[size_t(me.material)];
      if (m.char_class.size() != m.materials.size()) m.char_class.assign(m.materials.size(), 0);   // review P2-5: the class flags once per material, not per draw
      uint8_t& cls8 = m.char_class[size_t(me.material)];
      if (!(cls8 & 1)) {
        const technique_pick::CharClass cl = technique_pick::CharClassOf(read_rule::Lower(cm.shader));
        cls8 = uint8_t(1 | (cl.e12 ? 2 : 0) | (cl.e4 ? 4 : 0) | (cm.translucent_draw ? 8 : 0) | (cl.e20 ? 16 : 0));   // S111 LAYERS: bit 3 = the part is drawn in the character's TRANSLUCENT draw (Material::translucent_draw, the game's split: assets\char_draw_layer.h; not alpha_blend, which a texel cut-out also sets), which zeroes the shadow slot (sub_8259FAE8 +0x18C): no +2 on any path; was the hair NAME class
      }
      technique_pick::CharClass cl;
      cl.e12 = (cls8 & 2) != 0, cl.e4 = (cls8 & 4) != 0, cl.hair = (cls8 & 8) != 0, cl.e20 = (cls8 & 16) != 0;
      if (eng_obj != it.object) eng_obj = it.object, eng_mode = options_.census_char_mode >= 0 ? options_.census_char_mode : CharModeOf(frame, objs[it.object].frame_object), eng_row = ObjectEngineRowOf(frame, objs[it.object].frame_object);   // the FRAME index (liquid_join.h)
      const int mode = eng_mode;
      char_mode_unknown_ += mode < 0;   // counted per wrestler DRAW (a record that carries the mode makes it 0)
      if (wrk::Live()) {   // S103 WRK census: the pass mode (+616: 0 / 2 picks; +620 picks the +16 term below since S111 TECHPASS; the part's E+20 any-word is recorded and counted only)
        const SceneFrame::ObjectEngine* oe = eng_row;
        ++(mode == 0 ? wrk_.m0 : mode == 2 ? wrk_.m2 : mode < 0 ? wrk_.m_unknown : wrk_.m_other);
        const int m620 = oe ? oe->char_mode620 : -1;
        ++wrk_.m620[m620 < 0 || m620 > 3 ? 4 : m620];
        wrk_.e20_set += oe && oe->e20_any == 1, wrk_.e20_clear += oe && oe->e20_any == 0, wrk_.e20_unknown += !oe || oe->e20_any < 0;
        wrk_.e20_would_fire += oe && oe->e20_any == 1 && m620 == 1;   // the +16 pass of sub_82BD5EE0 would fire on some part
      }
      // S111 TECHPASS (docs\research\s111\TECHPASS.md section B): the +16 term keys on the game's own model +620 of this body (latched: inputs_core.h ReadModelRuntimeT, carried by wrk_join.h CarryBody);
      // -1 = no row / not latched (the switch off): no term, as before. A guest-routed SUBM piece (a created superstar's hair, kObjOwnRowsPiece) has a row of its own: it is a latch target and the game copies the word into every SUBM piece model (sub_825A1E38 +0xCC). The wrk census block above still counts the word.
      const int model620 = eng_row ? eng_row->char_mode620 : -1;
      const int pass = technique_pick::CharPassOf(cl, m.skinned_model, shadow_map && !cl.hair, mode, cm.spec_pow_prm, model620);   // S111 OLDCHAIN review: "model is skinned" (the game's model +540) is the model's own flag, not this frame's palette
      // S108 THR2: a body the game draws recoloured this frame (the record's section 67 -> SceneFrame::change_colours) whose material has its masks binds the STS row (change_colour_draw.h)
      const float* ccol = frame.change_colours.empty() ? nullptr : change_colour_draw::ConstantsOf(frame.change_colours, objs[it.object].frame_object);   // the FRAME index (as the liquid map's join)
      const bool want_cc = change_colour_draw::WantsChangeColour(ccol, cm);
      const Bound& cb = BindMaterial(m, me.material, me.skinned && me.chunk, LmMode::Own, false, false, pass, false, want_cc);
      if (cb.char_table && !cb.debug) char_b = &cb;
      else char_hole = &cb, ++char_hole_draws_;   // S111 OLDCHAIN: a hole (BindMaterial counted its reason per bind)
      if (want_cc) {
        if (char_b && char_b->change_colour) char_cc = ccol, change_colour_draw::Draws().draws.fetch_add(1, std::memory_order_relaxed);
        else change_colour_draw::Draws().no_row.fetch_add(1, std::memory_order_relaxed);   // no STS row for the effect / not bindable: the startup row, plain (a hole: not drawn)
      }
    }
    const Bound* plane_bound = nullptr;
    const Bound* light_bound = nullptr;   // S103 RML
    if (!rule_bound) {
      const bool ent_lm = frame.entrance && me.material >= 0 && EntranceLmClass(m.materials[size_t(me.material)], frame.entrance_bg) && !IsMovieSurface(me.part);  // S101 lane 15
      // S103 L5 (docs/formats/arena_slot_flags.md "S103 L5"): the game reads the stage page for EVERY non-translucent arena yBG_Default / yBG_UVScroll / yProp_Default mesh, in a match as in an entrance
      // (arena 4: 157 of 157 opaque yBG_Default + 16 yProp_Default non-`a` / non-`s` meshes the streams show take an LM variant, whether the file has a sampLightMapPrm or not; the `a` / `s` slots (alpha_blend here) are the plain ones).
      // Without the page our stairs / tier walls / exit frame (no own light map) were drawn plain = unshaded, 1.5-2x the game's in a match.
      const bool arena_lm = !frame.entrance && me.slot_no >= 0 && me.material >= 0 && ArenaMatchLm(me.part, m.materials[size_t(me.material)]);
      // S103 L10 (docs/formats/arena_lightmap.md "S103 L10"): CORRECTS the L5 line above for a MATCH. The game's match stream (run 27: 105 yBG_Default draws of frame 500, the same split in frames 100 / 300 / 641) draws the arena with the plain
      // `yBG_Default` family: 46 draws take `yBG_Default_vertex_lm_vertex` + `_base_fragment` (slots 0 and 16: the page in the VERTEX stage only), 59 take `_base_vertex` + `_base_fragment` (slot 0 only: no page at all).
      // The class is the mesh record's word +0x90 bit 0 (Material::vertex_lm, 264 / 264). So a match arena mesh is lm_mode 2 (vertex page only) or 3 (no page); the LM family with the page per pixel is the ENTRANCE's.
      lm_mode = (ent_lm || arena_lm) ? LmMode::EntranceLm : LmMode::Own;
      if (!frame.entrance && me.slot_no >= 0 && me.material >= 0) {   // only the yBG_Default class the stream proves; every other class keeps the L5 rule above
        const assets::Material& am = m.materials[size_t(me.material)];
        if (ArenaMatchVertexPage(me.part, am)) lm_mode = LmMode::MatchVertexPage;
        else if (ArenaMatchNoPage(me.part, am)) lm_mode = LmMode::MatchNoPage;
      }
      const bool is_plane_mat = plane_map && me.material >= 0 && IsPlaneReaderEffect(m.materials[size_t(me.material)].shader);   // S103 L9
      plane_bound = is_plane_mat ? &BindMaterial(m, me.material, me.skinned && me.chunk, lm_mode, true) : nullptr;
      // S103 RML (RMP.md 1.4 item 6): the game's ring-mat draw (sub_82608CB0) takes the props-map pass (light_shadow) when sub_827ECB60 == 0 (no plane module, or [0x836F39A0] != 0): the soft contact blob under every wrestler on the mat.
      // Needs the props map (wwe2k14_rdr_props_map on + a map this frame); a frame where the module exists but ours made no plane map stays plain (the game drew screen_shadow there).
      const bool is_light_mat = !plane_map && props_on && props_img && me.material >= 0 && IsPlaneReaderEffect(m.materials[size_t(me.material)].shader) && frame.plane_game_off == 1 && !options_.plant_mat_plain_when_no_module;   // (the plant = today's binding: no shadow at all)
      light_bound = is_light_mat ? &BindMaterial(m, me.material, me.skinned && me.chunk, lm_mode, true, false, -1, true) : nullptr;
    }
    // S103 YCB (review P2-1) / S111 GEARLIQUID: a table-picked character pair binds the body's liquid map (MapRole::SweatMap, keyed by the body's engine object) where ITS pixel variant reads slot 14; wet / dry is the pass's.
    // Skin and gear alike: the game binds the character's own map at 14 once per body-model draw (liquid_gate.h); a body with no map this frame keeps the black stand-in (dry). S111 OLDCHAIN: the table pick is the only pick.
    // GEARLIQUID review: only a BODY mesh (src_key 0): a stock wrestler's pieces are merged into the body's object here, but the game's SUBM pieces get no callback (they sample a stale slot 14).
    const MapImage* liquid = (char_b && liquid_gate::TakesBodyMap(is_char, me.material, char_b->liquid_slot, me.src_key)) ? liquid_of(objs[it.object].frame_object) : nullptr;   // the FRAME index, not the dense one (review SWF-1)
    // S108 THB: an entrance PROP the game draws recoloured this frame (the record's section 67 row of kCcProp -> SceneFrame::change_colours, carried by rf_props.cpp): its plain bind is the STS cc_mask row with the prop's mask at slot 12.
    // A material with no mask loaded (the id has no SPMT row / the file lacks it) draws plain and is counted; a row the STS table cannot bind draws plain too (`no row`).
    const SceneFrame::ChangeColour* prop_cc_entry = (!is_char && me.material >= 0 && !frame.change_colours.empty()) ? change_colour_draw::EntryOf(frame.change_colours, objs[it.object].frame_object) : nullptr;
    if (prop_cc_entry && !change_colour_draw::IsPropEntry(prop_cc_entry->flags)) prop_cc_entry = nullptr;   // (a wrestler body's row: not ours)
    const bool prop_cc_want = prop_cc_entry && !char_b && !rule_bound && !(plane_bound && plane_bound->plane) && !(light_bound && light_bound->props_reader);
    if (prop_cc_want && !change_colour_draw::PropHasMask(m.materials[size_t(me.material)]) && (prop_cc_entry->flags & kCcBeltMask) == 0) live::propcolour::TheStore().NoteNoMask();   // S111 BELTMASKNAME review (F4): a belt row's materials off key 0 never take a mask and a pending one is counted by [belt_colour] (masks asked / not resident): not a THB `no mask`
    const bool prop_cc_ask = prop_cc_want && change_colour_draw::PropHasMask(m.materials[size_t(me.material)]);
    const Bound& b = char_b ? *char_b : char_hole ? *char_hole : rule_bound ? *rule_bound : (plane_bound && plane_bound->plane) ? *plane_bound : (light_bound && light_bound->props_reader) ? *light_bound : BindMaterial(m, me.material, me.skinned && me.chunk, lm_mode, false, false, -1, false, prop_cc_ask);   // S111 OLDCHAIN: a wrestler draw is char_b or char_hole, never a pass -1 bind
    const bool prop_cc_draw = prop_cc_ask && b.change_colour && !b.chr;   // the bind is the STS row (a fallback pick leaves change_colour false: drawn stock, `no row` would be the finding)
    if (prop_cc_draw) live::propcolour::TheStore().NoteDrawn();
    else if (prop_cc_ask) live::propcolour::TheStore().NoteNoRow();
    // S105 SZW (docs/research/harness/SZW_contract.md): the game alpha-tests every stage draw (GEQUAL 1/255), so a stage draw that writes depth MUST discard (rule 2: never a depth write without a discard): the bound pair's alpha twin
    // is used for it, and a pair with no twin keeps depth write OFF (counted, K of the census line). A `z`-only slot is tested too (it writes no depth). zw_item = the rule applies to this draw (switch on, an `s` / `a` / `z` slot or a blended screen piece).
    const bool zw_item = zw_on && it.zw != 0 && !is_char;
    if (zw_item && b.zw_twin && !b.debug && !b.zw_program) {   // S105 SZW (review 2b): the twin programs are made when the switch is first seen on, not at bind
      b.zw_program = ProgramFor(b.choice.vs, b.choice.ps, true);
      if (plant_zw_no_test) b.zw_plain = ProgramFor(b.choice.vs, b.choice.ps, false);
    }
    if (zw_item && b.zw_twin && !b.debug && plant_zw_no_test && !b.zw_plain) b.zw_plain = ProgramFor(b.choice.vs, b.choice.ps, false);
    const bool zw_test = zw_item && !plant_zw_no_test && (it.depth_write || (it.zw & 3) == 2);   // (an `s z` / `a z` draw writes no depth: blended alpha ~0 there is invisible, no test needed)
    const bool zw_discard = zw_test && !b.debug && b.zw_twin;                                      // the draw really discards (the alpha twin is bound)
    const bool plant_strip = zw_item && plant_zw_no_test && !b.debug && b.zw_twin && (it.depth_write || (it.zw & 3) == 2);   // the plant strips the test of EVERY stage draw that writes depth (the unsafe direction)
    // S108 HAIR (piece_draw_rule.h): a stock piece's translucent mesh discards below 128/255 and writes depth, through the pair's alpha twin (made on first use, the zw_program slot: one twin per pair). No twin = no discard possible =
    // NO depth write (the SZW rule: never a depth write without a discard), reported once per pair as a draw_shortfall. The planted twin g_plant_piece_cut_no_test (TEST ONLY) keeps the depth write with no discard.
    const bool pc_item = it.piece_cut;
    const bool pc_twin = pc_item && !b.debug && VariantHasTwin(b.choice.ps) && !plant_pc_no_twin;
    if (pc_twin && !b.zw_program) b.zw_program = ProgramFor(b.choice.vs, b.choice.ps, true);
    const bool pc_discard = pc_twin && !plant_pc_no_test;
    // S111 CHALPHA (char_alpha.h; 360_HABITS.md row 32): a tint-only wrestler pair (the bind left it untested) is tested + blended on a draw whose MdlAlpha / MatAmbCol.w / MatDifCol.w (the values
    // ResolveChar pushes) are not all exactly 1, as the game's category reset sub_82C22E88 tests every one; at all three 1 its alpha is 1 and the test and blend change no pixel. The twin rides the
    // zw_program slot, made on first use as the piece cut's (a character bind is never a zw_item).
    const assets::Material* tint_mat = (char_b && !b.debug && !b.alpha_test && me.material >= 0) ? &m.materials[size_t(me.material)] : nullptr;
    const bool tint_discard = tint_mat && char_alpha::DrawTests(kVariants[b.choice.ps].out_alpha, VariantHasTwin(b.choice.ps), it.alpha, tint_mat->ambient[3], tint_mat->diffuse_color[3]);
    if (tint_discard && !b.zw_program) b.zw_program = ProgramFor(b.choice.vs, b.choice.ps, true);
    const bool at_on = !plant_strip && (b.alpha_test || zw_discard || pc_discard || tint_discard);                    // the draw's alpha test as the state / dynamic / dump see it
    const rdr::ImageId liquid_image = liquid ? liquid->image : rdr::kNoImage;   // (liquid is set only for a table-picked character pair that reads slot 14)
    // S103 WGD (dev, WWE_RDR_FEATURE_DUMP; docs\agents\choice_diff.md): ONE "d2" line per mesh and look = what this chunk REALLY drew with, read where the draw is final (the pick
    // order of the line above, the blend / alpha test / depth write / cull of the draw state below): tools\harvest\choice_ours.py joins it with the game's rule. Read-only: changes no draw.
    // One line per mesh per Record (d2_seen); none from the seed frame's Record (in_seed_: RenderToPixels' extra no-post frame, before the frame the tool reads).
    // The alpha inputs (vertex / pixel out_alpha, the vertex-colour fetch, MdlAlpha, MatAmbCol.w / MatDifCol.w, the texture's alpha share) let the tool prove the output alpha 1.
    const auto d2 = [&](const rdr::DrawState* st, const rdr::DynamicState* dy, const rdr::fx::Variant* vvp, const rdr::fx::Variant* fvp, const std::vector<std::pair<int, rdr::SamplerDesc>>* samp) {
      FILE* f = fdump::File();
      if (!f || in_seed_) return;
      if (!d2_seen.insert({&me, it.mirrored}).second) return;
      const int out_alpha = fvp ? int(fvp->out_alpha) : -1;
      std::string sj = ",\"samp\":{";   // per sampler slot as bound: address u / v / w, mag / min filter, mip mode (rdr::SamplerDesc numbers)
      for (size_t i = 0; samp && i < samp->size(); ++i) {
        const rdr::SamplerDesc& s = (*samp)[i].second;
        sj += (i ? ",\"" : "\"") + std::to_string((*samp)[i].first) + "\":[" + std::to_string(int(s.address_u)) + "," + std::to_string(int(s.address_v)) + "," + std::to_string(int(s.address_w)) + "," +
              std::to_string(int(s.mag_filter)) + "," + std::to_string(int(s.min_filter)) + "," + std::to_string(int(s.mip_mode)) + "]";
      }
      sj += "}";
      const char* path = b.debug ? "unbound" : char_b ? "char" : rule_bound ? "rule" : (plane_bound && plane_bound->plane) ? "plane" : (light_bound && light_bound->props_reader) ? "light" : "plain";
      const assets::Material* dm = me.material >= 0 ? &m.materials[size_t(me.material)] : nullptr;
      const bool at = st && at_on;
      char abuf[200];
      std::snprintf(abuf, sizeof abuf, ",\"vs_oa\":%d,\"vc\":%d,\"mdl_a\":%.4f,\"amb_w\":%.4f,\"dif_w\":%.4f,\"dal\":%.4f", vvp ? int(vvp->out_alpha) : -1, vvp ? int(vvp->vc_fetch) : -1, double(it.alpha),
                    dm ? double(dm->ambient[3]) : -1.0, dm ? double(dm->diffuse_color[3]) : -1.0, dm ? double(dm->diffuse_alpha_clear) : -1.0);
      char buf[900];
      std::snprintf(buf, sizeof buf, ",\"blend\":%d,\"ben\":%d,\"bdst\":%d,\"at\":%d,\"af\":%d,\"ar\":%d,\"dw\":%d,\"cull\":%d,\"ff\":%d,\"mir\":%d,\"cword\":%u,\"oa\":%d,\"lm\":%d,\"phase1\":%d,\"ent\":%d,\"grp\":%d,\"rule_tried\":%d,"
                    "\"spec_lev\":%.3f,\"no_hl\":%d,\"ablend\":%d,\"add\":%d,\"inl\":%d,\"cutout\":%d,\"mat_at\":%d,\"amask\":%d,\"cull_back\":%d,\"szw\":%d,\"twin\":%d}\n",
                    st ? int(it.blend) : -1, st ? int(st->blend[0].enable) : -1, st ? int(st->blend[0].dst_color) : -1, at ? 1 : 0, at && dy ? int(dy->system[9]) : -1, at && dy ? int(dy->system[8] * 255.f + 0.5f) : -1,
                    st ? int(st->depth_write) : -1, st ? int(st->cull) : -1, st ? int(st->front_face) : -1, it.mirrored ? 1 : 0, dm ? unsigned(dm->cull_word) : 0u, out_alpha, int(lm_mode), phase1 ? 1 : 0, frame.entrance ? 1 : 0, int(m.group),
                    (!is_char && m.arena_id >= 0 && me.slot_no >= 0 && me.slot_no <= 398 && me.material >= 0) ? 1 : 0, dm ? double(dm->spec_lev) : 0.0, dm && dm->no_highlight ? 1 : 0,
                    dm && dm->alpha_blend ? 1 : 0, dm && dm->additive ? 1 : 0, dm && dm->blend_inline ? 1 : 0, dm ? int(dm->cutout_blend) : 0, dm && dm->alpha_test ? 1 : 0, dm && dm->alpha_mask ? 1 : 0, dm && dm->cull_back ? 1 : 0, int(it.zw), b.zw_twin ? 1 : 0);   // S105 SZW: the slot state bits, and whether the pair has an alpha twin
      const std::string js = "{\"k\":\"d2\",\"part\":\"" + fdump::Esc(me.part) + "\",\"slot\":" + std::to_string(me.slot_no) + ",\"mesh\":" + std::to_string(me.src_record) + ",\"key\":" + std::to_string(me.src_key) +
                             ",\"src_pac\":\"" + fdump::Esc(me.src_pac) + "\",\"arena\":" + std::to_string(m.arena_id) + ",\"shader\":\"" + fdump::Esc(dm ? dm->shader : std::string("(no material)")) + "\",\"vs\":\"" +
                             (b.debug ? "" : kVariants[b.choice.vs].name) + "\",\"ps\":\"" + (b.debug ? "" : kVariants[b.choice.ps].name) + "\",\"path\":\"" + path + "\"" + sj + abuf + buf;
      std::fwrite(js.data(), 1, js.size(), f);
      std::fflush(f);   // a served rig keeps the file open between jobs: the job's lines are on disk when its @@JOB_END is read
    };
    if (b.debug && (!options_.debug_unbound || debug_vs_ < 0)) { d2(nullptr, nullptr, nullptr, nullptr, nullptr); ++coverage_[me.material >= 0 ? m.materials[size_t(me.material)].shader : "(no material)"].meshes_unbound; return; }
    if (b.props_reader) ++props_mat_bound_;   // S103 RML
    else if (!b.plane && !is_char && me.material >= 0 && IsPlaneReaderEffect(m.materials[size_t(me.material)].shader)) ++plane_wanted_plain_;   // S103 MSF: a ring-mat draw that wanted the plane-shadow variant and got a plain one (no map this frame, or the rule pair is not a plane reader)
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
    // S103 L (dev diagnosis only): WWE_RDR_L_SET="c00 c01 c02 c10 .. c32 sky0 sky1 sky2 gnd0 gnd1 gnd2" (18 floats, light colours then sky, ground) feeds every non-character draw that light set (the game's own tuple of the frame)
    static const assets::LightSet* diag_set = []() -> const assets::LightSet* {
      const char* e = std::getenv("WWE_RDR_L_SET");
      if (!e) return nullptr;
      static assets::LightSet d = assets::DefaultRig();
      float v[18] = {};
      const char* p = e;
      for (int i = 0; i < 18 && *p; ++i) { char* q = nullptr; v[i] = std::strtof(p, &q); if (q == p) break; p = q; }
      for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) d.col[k][i] = v[k * 3 + i];
      for (int i = 0; i < 3; ++i) d.sky[i] = v[12 + i], d.ground[i] = v[15 + i];
      return &d;
    }();
    const assets::LightSet& ls = (diag_set && !b.chr) ? *diag_set : (set_index >= 0 && size_t(set_index) < sets.size()) ? sets[size_t(set_index)] : default_rig;
    double item_time = frame.time_s;   // S103 E5: c186 is the game's per-slot, per-part clock, not one scene time
    if (frame.part_clock_count && me.uv_scroll && me.slot_no >= 0) {
      float c = 0.f;
      if (ClockOfSlot(frame.part_clocks, frame.part_clock_count, uint32_t(me.slot_no), me.src_record, c)) item_time = double(c);
      else if (frame.time_s == 0.0) ++time_frozen_;   // S103 RCL: no clock row for this draw
    } else if (me.uv_scroll && frame.time_s == 0.0) {
      ++time_frozen_;   // S103 RCL: a Time-reading draw with no part-clock rows at all (or no slot): the live record's time_s is never set, so its Time stays 0, counted, never silent
    }
    Ctx ctx{it.world.data(), vp, eye, item_time, &mat, &ls, m.lm_xy, m.lm_gain, it.alpha};
    ctx.plant_vs_prm_ps_rows = options_.plant_vs_prm_ps_rows;   // S103 RML planted twin (pms_check.py)
    const MapImage* rd_map = (b.chr || (b.props_reader && options_.plant_mat_reads_mapb)) ? shadow_map : floor_map;   // (S103 RML plant mat_reads_mapb: the mat reads the wrestlers' map B)   // S103 PMS: the map this draw's slot 12 reads (a character: the wrestlers' map B; a floor receiver: the props map, or nothing = the white stand-in)
    if (b.plane) ctx.plane_m = plane_m, ++plane_bound_;   // S103 L9
    ctx.mirror = (frame.viewer_space && !options_.plant_no_mirror) ? -1.f : 1.f;   // S103 RFL: the viewer's space is the game's with y negated: the shader flips the cube lookup back (ReflectAlpha.y)
    if (b.cube_real) ctx.reflect_alpha = mat.reflect_alpha, ++reflect_draws_;   // S103 RFL: slot 8 = the model's file cube: ReflectAlpha = g_fReflectAlpha
    if (prop_cc_draw) FeedPropColour(ctx, prop_cc_entry->constants, prop_cc_entry->flags);   // S108 THB: g_fChangeColor of an entrance prop (the 4 colours), MatDifCol 1 under the editor's callback
    ctx.amb_rgb = objs[it.object].amb_rgb;   // S111 CAARING: a created arena's rope: its own MatAmbCol rgb
    if (((rule_bound && b.shadow) || b.props_reader) && floor_map) ctx.shadow_m = shadow_bg_m, ctx.shadow_prm = &floor_map->prm[0][0], props_bound_ += props_on, pms_site_[4] += props_on;   // site 4: matrix and prm from the props map
      // S103 D1; S103 PMS: the floor's map
    bool mats_line_due = false;   // S111 TOOLFIX3: set by the dump block below for a material not yet dumped; the [mats] line is printed at the end of the draw's state
    if (static const bool dump = std::getenv("WWE_RDR_DUMP_MATS") != nullptr; dump && !b.debug && me.material >= 0) {  // S101 lane 257 (dev): one line per distinct material: what the draw is fed
      static std::set<const void*> seen;
      if (seen.insert(&me).second) {
        mats_line_due = true;
          // S111 TOOLFIX3: the [mats] line itself is printed below, where the draw state is final (after the blend / alpha-test state is built), so it shows the draw's real state
        // S103 L (dev, WWE_RDR_DUMP_MATS): the whole light set this mesh is fed (set id, 4 colours, sky, ground) + the light-map gain, to compare with the game's draw constants
        std::fprintf(stderr, "[mats-lit] %s part=%s set_idx=%d id=%d col=[%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f %.4f %.4f|%.4f %.4f %.4f] sky=%.4f %.4f %.4f gnd=%.4f %.4f %.4f lmgain=%.2f stage_lm=%d\n", mat.shader.c_str(), me.part.c_str(), set_index, set_index >= 0 && size_t(set_index) < sets.size() ? sets[size_t(set_index)].id : -9,
                     ls.col[0][0], ls.col[0][1], ls.col[0][2], ls.col[1][0], ls.col[1][1], ls.col[1][2], ls.col[2][0], ls.col[2][1], ls.col[2][2], ls.col[3][0], ls.col[3][1], ls.col[3][2], ls.sky[0], ls.sky[1], ls.sky[2], ls.ground[0], ls.ground[1], ls.ground[2], m.lm_gain, m.stage_lm);
        float v[16] = {};   // S103 RL (dev): the DifLghVec this draw binds (after the camera-attached turn), in the draw's space
        Resolve("DifLghVec", 4, ctx, v);
        std::fprintf(stderr, "[mats-vec] part=%s id=%d follow=%d%d%d%d/%d amb=%.2f dif=%.2f v0=%.3f %.3f %.3f v1=%.3f %.3f %.3f v2=%.3f %.3f %.3f v3=%.3f %.3f %.3f\n", me.part.c_str(), ls.id, int(ls.follow[0]), int(ls.follow[1]), int(ls.follow[2]), int(ls.follow[3]),
                     int(ls.follow_view), mat.ambient[0], mat.diffuse_color[0], v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10], v[12], v[13], v[14]);
      }
    }
    // S103 CAWB (release, capped): one line per distinct uv_scroll / additive mesh + light set: the constants this draw is fed (CAWV Q2: which one makes the CAS_moya cards too bright: light set, MdlAlpha or the material's w).
    if ((me.uv_scroll || it.blend == 2) && !b.debug && me.material >= 0) {
      static std::mutex mu;
      static std::set<std::pair<const void*, int>> seen;
      static int lines = 0;
      std::lock_guard<std::mutex> lk(mu);
      if (lines < 16 && seen.insert({&me, set_index}).second) {
        ++lines;
        float us[4] = {};
        Resolve("UVScroll0", 4, ctx, us);
        char pic[128] = "none";   // S103 CAWS (CAWG rows 12, 14): slot 0's picture: material index, size, mips, source format, image id (0 = no image)
        if (const int di = mat.diffuse; di >= 0 && size_t(di) < m.img_info.size() && size_t(di) < m.images.size())
          std::snprintf(pic, sizeof pic, "%d %ux%u mips=%u fmt=%d image=%u rgba8=%d", di, m.img_info[size_t(di)].w, m.img_info[size_t(di)].h, m.img_info[size_t(di)].mips, m.img_info[size_t(di)].fmt, unsigned(m.images[size_t(di)]), int(m.img_info[size_t(di)].rgba8));
        rdr::Log("[scene][addmat] picture=%s part=%s slot=%d set_idx=%d set_id=%d blend=%d sorted=%d MatAmbCol=%.3f %.3f %.3f %.3f MatDifCol=%.3f %.3f %.3f %.3f ambw=%.3f difw=%.3f MdlAlpha=%.3f time=%.3f UVScroll0=%.4f %.4f %.4f %.4f DifLghCol=[%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f|%.3f %.3f %.3f] hemi sky=%.3f %.3f %.3f gnd=%.3f %.3f %.3f", pic, me.part.c_str(), me.slot_no, set_index, set_index >= 0 && size_t(set_index) < sets.size() ? sets[size_t(set_index)].id : -9, int(it.blend), int(it.sorted),
                     mat.ambient[0], mat.ambient[1], mat.ambient[2], mat.ambient[3], mat.diffuse_color[0], mat.diffuse_color[1], mat.diffuse_color[2], mat.diffuse_color[3], mat.ambient[3], mat.diffuse_color[3], double(it.alpha), item_time, us[0], us[1], us[2], us[3],
                     ls.col[0][0], ls.col[0][1], ls.col[0][2], ls.col[1][0], ls.col[1][1], ls.col[1][2], ls.col[2][0], ls.col[2][1], ls.col[2][2], ls.col[3][0], ls.col[3][1], ls.col[3][2], ls.sky[0], ls.sky[1], ls.sky[2], ls.ground[0], ls.ground[1], ls.ground[2]);
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
          else if (oi.skin) ++identity_nodes_frame_;   // S103 CAWS (CAWG row 4): a node outside the palette is drawn with identity; counted per frame, printed by the [scene][identity] line
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
    cc.view = (frame.camera.flags & kCamViewIsViewProj) ? nullptr : frame.camera.view.m;   // S111 VIEWROWS review: the game's own world -> view for View (c52-c54); a combined record has none
    if (b.chr && rim_map) cc.rim_params = rim_map->params;   // S103 L7
    cc.spec_sweat = b.char_table;   // S103 YCB
    cc.cube_analytic = b.cube_analytic;
    cc.wet = b.wet;   // S103 SWF: the material's wet constants (c205.zw, c210, c234, c246) only for a wet-bound draw
    cc.change_color = (b.change_colour && &b == char_b) ? char_cc : nullptr;   // S108 THR2: g_fChangeColor of a change-colour bind (the body's colours this frame)
    const bool wrk_deg = mat.has_wrink_uv && wrk::UvDegenerate(mat.wrink_uv);   // S103 XLF: a degenerate wrinkle rectangle (shared by the join and the census)
    float wrk_wkl[6], wrk_idmap[8];   // S103 WRK: this draw's live levels (the body's rows, the part's own over the model-wide)
    // Levels only for a BODY mesh (src_key 0: the pac's entry 0; a piece's part index would collide, review F3), only when the bound vertex variant declares the level fields (b.wrk_fields, F4), and not for a degenerate
    // wrinkle rectangle (F2: the draw then has no wrinkle depth, counted).
    if (b.chr && !b.debug && wrk::Live() && b.wrk_fields && wrk::JoinsBodyLevels(me.src_key)) {
      // S103 XLF: a degenerate rectangle drops only the wrinkle depth (wkl); the id-map tint levels do not depend on the rectangle and are still joined.
      if (wrk_deg) ++wrk_.uv_degenerate;
      wrk::JoinDraw(frame.wrinkle_levels, objs[it.object].frame_object, me.src_record >= 0 ? uint32_t(me.src_record) : kParamModelWide, wrk_deg, wrk_wkl, wrk_idmap, cc.wkl, cc.idmap);
    }
    // S111 MATFEED: the body's live g_fHdLigLvl / g_f4MuscleParam rows (wrk_join.h MergeLive: the model-wide over the part's own, as the walk; HdLig.x only for a material naming g_fHdLigLvl) for HdLig.x (c156)
    // and MuscleParam (c249); none = the file values. (A draw whose material does not name it then neither takes nor stamps the body's value.)
    float live_hd_lig = 0.f, live_muscle[4] = {0.f, 0.f, 0.f, 0.f};
    if (b.chr && !b.debug && wrk::Live() && wrk::JoinsBodyLevels(me.src_key)) {
      const uint32_t got = wrk::MergeLive(frame.wrinkle_levels, objs[it.object].frame_object, me.src_record >= 0 ? uint32_t(me.src_record) : kParamModelWide, mat.has_hd_lig_lvl, live_hd_lig, live_muscle);
      if (got & 1u) cc.hd_lig_lvl = &live_hd_lig;
      if (got & 2u) cc.muscle = live_muscle;
    }
    if (b.shadow && rd_map) cc.shadow_matrix = rd_map->matrix, cc.shadow_params = rd_map->params, cc.shadow_prm = &rd_map->prm[0][0], shadow_raw_bound_ += rd_map->raw;   // S103 L6; S103 PMS: rd_map
    if (b.chr && !b.debug) cc.sphere_occ = sphere_join::Of(frame.body_spheres, objs[it.object].frame_object);   // S111 SPHEREOCC: the body's recorded spheres (FRAME index), null = strength 0

    const int vs = b.debug ? debug_vs_ : b.choice.vs, ps = b.debug ? debug_ps_ : b.choice.ps;
    const rdr::fx::Variant& vv = kVariants[vs];
    const rdr::fx::Variant& fv = kVariants[ps];
    if (b.chr && !b.debug && wrk::Live() && b.reads_wrinkles) {   // S103 WRK census: a wrinkle-reading draw, and whether the record carried its levels
      wrk::CountWrinkleDraw(wrk_deg, cc.wkl, wrk_.wrinkle_draws, wrk_.levels_missing, wrk_.wkl_nonzero);   // (a degenerate rectangle has no wkl: counted as uv_degenerate, not as missing levels)
    }
    if (b.chr && !b.debug && wrk::Live() && cc.idmap && b.reads_idmap) { for (int i = 0; i < 8; ++i) if (cc.idmap[i] > 0.f) { ++wrk_.idmap_nonzero; break; } }
    // S103 lane G: this draw's kept list (SCENE_PACING.md 3b): which stamps changed since its last frame decides which constant fields are resolved again.
    KeptDraw* kept = nullptr;
    bool kept_fresh = false;
    uint32_t kept_changed = kStAll;
    if (draw_lists_ && !b.debug) {
      kept = &draw_lists_->Acquire({oi.id, oi.asset, uint32_t(&me - m.meshes.data()), it.inst, it.pass}, kept_fresh);
      if (kept->bind_key != &b) {  // a different binding (program / variants): everything is built again (counted as built)
        if (!kept_fresh) draw_lists_->NoteRebuilt();
        kept->bind_key = &b, kept_fresh = true;
      }
      Stamps st;
      st.h[0] = HashBytes(it.world.data(), sizeof(float) * 16, frame.viewer_space ? 1u : 2u);  // (+ the record's space rule: the characters' mirror)
      st.h[1] = slots.empty() ? 0 : HashBytes(slots.data(), slots.size() * sizeof(assets::Matrix));
      if (cc.sphere_occ) st.h[1] = HashBytes(cc.sphere_occ, sizeof(float) * 16, st.h[1]);   // S111 SPHEREOCC: the spheres move SphereOccP / SphereOccV (kStPalette, draw_lists.cpp)
      st.h[2] = cam_stamp;
      st.h[3] = item_time == frame.time_s ? time_stamp : HashBytes(&item_time, sizeof(double));   // S103 E5: the mesh's own clock
      st.h[4] = light_stamp(b.chr && char_light ? char_light : &ls);
      // S103 RML (review F1): EVERY Bound flag that makes the draw read ctx.shadow_m / shadow_prm (b.shadow: the floor; b.props_reader: the ring mat's light pass) must be in this stamp (tools\lint\light_stamp_lint.py).
      if ((b.shadow || (b.props_reader && !options_.plant_light_stamp_no_mat)) && rd_map) st.h[4] = HashBytes(rd_map->prm, sizeof rd_map->prm, HashBytes(rd_map->matrix, sizeof rd_map->matrix, HashBytes(rd_map->params, sizeof rd_map->params, st.h[4])));   // S103 L6; S103 PMS: rd_map
      if (b.plane && plane_map) st.h[4] = HashBytes(plane_m, sizeof plane_m, st.h[4]);   // S103 L9: the plane map's projection follows the camera
      if (b.chr && rim_map) st.h[4] = HashBytes(rim_map->params, sizeof rim_map->params, st.h[4]);   // S103 L7: the rim map's depth range moves RimLightDepth
      const float mv[4] = {it.alpha, m.lm_xy[0], m.lm_xy[1], m.lm_gain};
      st.h[5] = HashBytes(mv, sizeof mv, (uint64_t(uintptr_t(&m)) << 1) ^ uint64_t(me.material + 2));
      st.h[5] = wrk::StampLevels(cc.wkl, cc.idmap, st.h[5], [](const void* d, size_t n, uint64_t h) { return HashBytes(d, n, h); });   // (each pointer on its own: XLF review B2)   // S103 WRK: the levels move the WklLvLst / IdMapLvLst fields (kStMaterial)
      if (cc.hd_lig_lvl) st.h[5] = HashBytes(cc.hd_lig_lvl, sizeof(float), st.h[5]);   // S111 MATFEED: a live g_fHdLigLvl / g_f4MuscleParam moves HdLig / MuscleParam (kStMaterial, draw_lists.cpp)
      if (cc.muscle) st.h[5] = HashBytes(cc.muscle, sizeof(float) * 4, st.h[5]);
      if (cc.change_color) st.h[5] = HashBytes(cc.change_color, sizeof(float) * change_colour_draw::kFloats, st.h[5]);   // S108 THR2: a picked colour moves ChangeColor (kStMaterial, draw_lists.cpp)
      if (ctx.change_color) st.h[5] = HashBytes(ctx.change_color, sizeof(float) * 4 * live::propcolour::kColours, st.h[5] ^ (ctx.dif_one ? 0x9E3779B97F4A7C15ull : 0ull));   // S108 THB: a picked / latched prop colour (and the editor's MatDifCol 1) moves ChangeColor / MatDifCol
      if (ctx.amb_rgb) st.h[5] = HashBytes(ctx.amb_rgb, sizeof(float) * 3, st.h[5] ^ 0x5851F42D4C957F2Dull);   // S111 CAARING: a rope's colour moves MatAmbCol (kStMaterial, draw_lists.cpp)
      kept_changed = DrawLists::Diff(*kept, st, kept_fresh);
    }
    uint32_t pushed_bytes = 0;   // S103 P7 (review F8): the size of the block pack() pushed last (the own blend tag reads it)
    const auto stash = [&](int variant, bool vs_side, const uint8_t* data, uint32_t bytes) {   // S103 RML (rig only: SetMatProbe): the ring mat's resolved blocks, by field, for the offline proof against the stream's constants
      if (!mat_probe_on_) return;
      if (vs_side) {   // the vertex block comes first: decides whether this draw is probed (the ring mat's draws, and the first 4 floor receivers of the read rule)
        const bool floor_recv = !b.props_reader && !b.plane && rule_bound && b.shadow;
        mat_probe_live_ = (b.props_reader || b.plane) ? mat_probes_.size() < 16 : (floor_recv && mat_floor_probed_ < 4);
        if (!mat_probe_live_) return;
        mat_floor_probed_ += floor_recv;
        mat_probes_.emplace_back();
        mat_probes_.back().props_reader = b.props_reader, mat_probes_.back().plane = b.plane, mat_probes_.back().floor = floor_recv;
        mat_probes_.back().slot12 = b.props_reader ? mat_probe_slot12_ : rdr::kNoImage;
      }
      if (!mat_probe_live_ || mat_probes_.empty()) return;
      MatProbe& mp = mat_probes_.back();
      (vs_side ? mp.vs : mp.ps) = kVariants[variant].name;
      const fxt::VariantTable* t = T().by_index[size_t(variant)];
      if (!t) return;
      for (uint32_t k = 0; k < t->field_count; ++k) {
        const fxt::Field& f = t->fields[k];
        if (f.offset + f.vec4s * 16u > bytes) continue;
        MatProbeField pf;
        pf.name = f.name, pf.vec4s = f.vec4s, pf.v.resize(size_t(f.vec4s) * 4);
        std::memcpy(pf.v.data(), data + f.offset, size_t(f.vec4s) * 16);
        (vs_side ? mp.vs_f : mp.ps_f).push_back(std::move(pf));
      }
    };
    const auto pack = [&](int variant, const rdr::fx::Variant& v, KeptBlock* kb, bool vs_side) {
      const fxt::VariantTable* t = T().by_index[size_t(variant)];
      const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(v.block_bytes, 16), 8192);
      pushed_bytes = bytes;
      if (kb) {  // S103 lane G: the kept block: built whole on a fresh draw, else only the fields whose stamps changed are resolved again
        if (kept_fresh) {
          kb->Begin(bytes);
          if (t)
            for (uint32_t k = 0; k < t->field_count; ++k)
              if (t->fields[k].offset + t->fields[k].vec4s * 16u <= bytes) kb->Add(t->fields[k].name, t->fields[k].offset, t->fields[k].vec4s);
        }
        draw_lists_->Refresh(*kb, kept_changed, [&](const KeptField& kf, float* out) {
          const std::string fname = kf.name;
          if (!(b.chr ? ResolveChar(fname, kf.vec4s, cc, out) : Resolve(fname, kf.vec4s, ctx, out, vs_side))) missing_fields_.insert(fname);
        });
        stash(variant, vs_side, kb->bytes.data(), uint32_t(kb->bytes.size()));
        return fb.PushConstants(kb->bytes.data(), uint32_t(kb->bytes.size()));
      }
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
          if (b.chr ? !ResolveChar(fname, f.vec4s, cc, out) : !Resolve(fname, f.vec4s, ctx, out, vs_side)) missing_fields_.insert(fname);
        }
      }
      stash(variant, vs_side, block.data(), bytes);
      return fb.PushConstants(block.data(), bytes);
    };

    rdr::DrawState state{};
    state.program = plant_strip ? b.zw_plain : (zw_discard || pc_discard || tint_discard) ? b.zw_program : b.program;   // S105 SZW: the alpha twin (the game's test) for a stage draw under the switch; S108 HAIR: and for a piece cut; S111 CHALPHA: and for a tint-only wrestler draw below alpha 1
    state.vertex_layout = b.layout;
    state.topology = 3;  // triangle list
    const assets::Material* mp = me.material >= 0 ? &mat : nullptr;
    // Back faces culled where the material says so: a stage mesh by its record's +0x8C bit 0x2 (SCU, jboy.h; sub_82C04DF0 0x82C0A880), the screens / crowd by their own rules; two-sided otherwise.
    // wwe2k14_rdr_stage_cull off (stage_cull_on, read once per Record) = the stage meshes two-sided again (today's draw); the screens keep theirs.
    // S105 C3: the same block draws every family: a stage slot's cull follows wwe2k14_rdr_stage_cull, every other family's wwe2k14_rdr_family_cull.
    state.cull = family_cull::Cull(mp, stage_cull_on, family_cull_on);
    // Clockwise front on screen (the game's value 6 = cull back + front CW for a normal camera, CUL_ADV C). The game swaps the winding for a bit-set mesh's `r` copy (0x82C0B920-0x82C0B968) so the same outer side
    // survives: the copy's front is counter-clockwise. In DrawState (not a dynamic state) so the colour cuts' depth pre-pass twin (renderer.cpp RecordPass) follows it. The plant (test only) leaves the swap out.
    // S105 C3: the `r` copy's swap is the stage slots' alone (a non-arena object with a mirrored world gets no swap in the game, CUL_ADV D). Planted twin (test only): the other families take the swap (a wrong winding).
    state.front_face = family_cull::FrontFace(mp, state.cull, it.mirrored, plant_no_flip, plant_family_flip);
    if (it.pass) state.cull = it.pass == 1 ? 2 : 1;  // S101 lane 13: hair / translucent, drawn twice: back faces culled, then front faces
    state.depth_test = 1;
    state.depth_write = it.depth_write ? 1 : 0;
    if (zw_item && state.depth_write && !zw_discard && !plant_zw_no_test && !((it.zw & 4) && b.debug)) state.depth_write = 0;   // (an UNBOUND screen piece keeps its old depth write: it draws nothing but the flat debug colour)   // S105 SZW rule 2: no discard (no alpha twin), no depth write
    if (it.zw && !in_seed_) {   // S105 SZW census: by the game's rule an `s` / `a` slot without `z` and a blended screen piece write depth; ours = the final state; nodisc = a depth write with no discard; notwin = a game-writer whose pair has no twin
      const bool game_writes = ((it.zw & 1) && !(it.zw & 2)) || (it.zw & 4);
      const bool has_discard = at_on && !b.debug && b.zw_twin;
      szw_.game += game_writes ? 1u : 0u;
      szw_.ours += state.depth_write ? 1u : 0u;
      szw_.nodisc += (state.depth_write && !has_discard) ? 1u : 0u;
      if (game_writes && (b.debug || !b.zw_twin)) ++szw_.notwin, szw_.notwin_names.insert(b.debug ? std::string("(unbound / debug)") : std::string(kVariants[b.choice.ps].name));
    }
    if (pc_item && !in_seed_) {   // S108 HAIR: the piece-cut census (`[scene][hair_cut]`) and the no-twin rule
      if (state.depth_write && !pc_discard && !plant_pc_no_test) {   // no alpha twin: the draw cannot discard below 128, so it must not write depth (the cards would cut what draws behind them)
        state.depth_write = 0;
        ++hc_.notwin;
        if (!b.pc_notwin_reported) {
          b.pc_notwin_reported = true;
          const std::string obj = m.label + " part " + me.part + " (" + mat.shader + ", pixel " + (b.debug ? std::string("unbound / debug") : std::string(kVariants[b.choice.ps].name)) + ")";
          rdr::DrawShortfall("piece_cut_notwin", obj.c_str(), 1, 0, "piece %d: the game alpha-tests it GEQUAL 128/255 and writes depth (sub_8259E488); this pair has no alpha twin, drawn blended with no test and no depth write", me.src_key);
        }
      }
      ++hc_.draws;
      hc_.cut += (state.depth_write && pc_discard) ? 1u : 0u;
      hc_.nodisc += (state.depth_write && !pc_discard) ? 1u : 0u;
    }
    const pass_state::PassClass pcls = pass_state::ClassOf(mat.shader.c_str(), it.pass != 0, pc_discard);   // S103 B2; S108 HAIR: a piece cut = see_through_cut (GEQUAL 128/255)
    state.depth_compare = pass_state::ForwardZ(pass_state::Of(pcls).game_z_func);  // S103 B2: the game's reversed GEQUAL -> our forward-Z LEQUAL (3, as before)
    state.blend[0].write_mask = 0xF;
    if (!it.blend && at_on && pass_state::Of(pcls).tested_blends && !options_.plant_cutout_noblend) {   // S103 B2 (review 1): an alpha-TESTED part is BLENDED by the game (census: every tested lit / arena draw has blend 1, 6 / 7, z write 1); with the 1/255 reference the test only drops alpha 0, the soft alpha is the blend's. An untested opaque draw stays unblended (alpha 1: invisible). The depth write and the draw order stay ours (opaque list)
      state.blend[0].enable = 1;
      state.blend[0].src_color = 6;  // src alpha
      state.blend[0].dst_color = 7;  // one minus src alpha
      state.blend[0].src_alpha = 1;
      state.blend[0].dst_alpha = 7;
    }
    if (it.blend) {
      state.blend[0].enable = 1;
      state.blend[0].src_color = 6;  // src alpha
      state.blend[0].dst_color = it.blend == 2 ? 1 : 7;  // one / one minus src alpha
      state.blend[0].src_alpha = 1;
      state.blend[0].dst_alpha = 7;
    }
    rdr::TextureSet set{};
    const bool d2_on = fdump::File() != nullptr;   // S103 WGD (dev): the samplers as bound, for the d2 line
    std::vector<std::pair<int, rdr::SamplerDesc>> d2_sd;
    for (uint32_t s = 0; !b.debug && s < fv.sampler_count; ++s) {
      const int slot = fv.samplers[s].slot;
      rdr::SamplerDesc sd{};
      sd.mag_filter = sd.min_filter = 1;
      sd.mip_mode = 1;
      sd.address_u = sd.address_v = sd.address_w = (b.slot_clamp[slot] || b.cube_slot[slot]) ? 2 : 0;
      sd.max_anisotropy = sampler_table::AnisoWanted();   // S103 B1: kAnisoFollowsSetting (false: 8x as before)
      sd.max_lod = 1000.f;
      TableSampler(b.samp, b.choice, slot, b.cube_slot[slot] == 4 || b.cube_slot[slot] == 5, false, sd);   // S103 B1
      if (live::change_colour::TakesMaskSampler(b.chr, b.change_colour, slot)) sampler_table::ChangeMaskSampler(sd);   // S110 ARMBAND: the Threads masks: point / clamp, one level (sub_8339F0E0); a prop's cc_mask bind is not one
      if (live::change_colour::TakesLogoSampler(b.chr, b.change_colour, b.wet, slot)) sampler_table::CawLogoSampler(sd);   // S110 LOGOSAMP: the CAW logos (Logo7 / Logo4): clamp, trilinear (sub_822E6CC8)
      if (live::change_colour::TakesPropMaskSampler(prop_cc_draw, prop_cc_draw && (prop_cc_entry->flags & kCcEditor) != 0, slot)) sampler_table::EditorPropMaskSampler(sd);   // S110 LOGOSAMP review: the Threads editor's prop mask (12): clamp, trilinear (sub_8338EC00); a saved set's match draw is not one
      if (d2_on) d2_sd.push_back({slot, sd});
      set.slots[slot].image = b.cube_slot[slot] == 1 ? (cube ? cube->image : b.slot_image[slot]) : (b.cube_slot[slot] == 4 && rd_map) ? rd_map->image : (b.cube_slot[slot] == 5 && plane_map) ? plane_map->image : (b.cube_slot[slot] == 2 && b.chr && rim_map) ? rim_map->image : b.slot_image[slot];  // the ambient cube is per object; S103 L6: slot 12 = the frame's shadow map
      if (b.cube_slot[slot] == 4 && !b.chr && !b.props_reader && rd_map && props_on && set.slots[slot].image == rd_map->image) ++pms_site_[5], pms_slot12_ = rd_map->image;   // site 5: slot 12 of a floor draw is the props map's image
      if (liquid_image != rdr::kNoImage && slot == int(b.liquid_slot)) set.slots[slot].image = liquid_image;   // S103 SWF: the wrestler's liquid map (wrap / linear / mip linear: the sampler of the effect's own table, not a clamp)
      if (slot == 12 && b.props_reader) mat_probe_slot12_ = set.slots[slot].image;   // S103 RML (rig): what the mat's slot 12 really holds, stored with its probe (rml_check.py: the props map's image, never map B's)
      set.slots[slot].sampler = fb.Sampler(sd);
      set.slots[slot].kind = fv.samplers[s].kind;
    }
    for (uint32_t s = 0; b.vtx_lm && !b.debug && s < vv.sampler_count; ++s) {  // S103 L10: the vertex stage's textures (slot 16 + i)
      const int slot = vv.samplers[s].slot;
      rdr::SamplerDesc sd{};
      sd.mag_filter = sd.min_filter = 1;
      sd.mip_mode = 1;
      sd.address_u = sd.address_v = sd.address_w = 2;
      sd.max_lod = 1000.f;
      TableSampler(b.samp, b.choice, slot, false, true, sd);   // S103 B1: the vertex light-map page (stage 16 + i: clamp in every effect that has it)
      if (d2_on) d2_sd.push_back({slot, sd});
      set.slots[slot].image = b.slot_image[slot];
      set.slots[slot].sampler = fb.Sampler(sd);
      set.slots[slot].kind = vv.samplers[s].kind;
      set.slots[slot].vertex = 1;
    }
    // S103 SCF [C, SCM.md sub_8276AEF8 / sub_8276A808]: a shown screen with no movie draws the live frame grab, never black: slot 800 always (match look), every other movie key the applier marked idle (SetIdleScreens; no look test here; the entrance look's idle screens are painted black by the applier and never marked)
    // WGS PAR1 (TEST ONLY): a primed grab (PrimeGrabForTest, the offline arena sheet) stands in for the live copy through this same binding; the game never primes it
    const rdr::ImageId grab_img = grab_prime_ != rdr::kNoImage ? grab_prime_ : live_screen_;
    if ((live_valid_ || grab_prime_ != rdr::kNoImage) && (me.part == "slot 800" || (me.slot_no >= 801 && me.slot_no <= 809 && (m.idle_screens >> (me.slot_no - 800) & 1u))) && grab_img != rdr::kNoImage && fv.sampler_count && set.slots[0].image != rdr::kNoImage) set.slots[0].image = grab_img;  // S101 lane 257
    rdr::DrawOp op{};
    op.state = fb.State(state);
    op.dynamic = fb.Dynamic(at_on ? alpha_dyn(pcls) : dynamic);
    op.textures = fb.Textures(set);
    op.vs_constants = pack(vs, vv, kept ? &kept->vs : nullptr, true);  // S103 G
    const uint32_t vs_pushed = pushed_bytes;
    op.fs_constants = pack(ps, fv, kept ? &kept->fs : nullptr, false);
    op.vertex[0] = {PosedVertexBuffer(m), me.vb_offset};   // S103 PF2: `vb`, or the mapped copy that holds the latest pose
    if (m.pcopy_cur >= 0) fb.NoteDirectRecord(renderer_.RenderSerial() + 1);   // the frame carries the serial it was recorded at: Render checks it is not rendered later than kRendersPerRecord
    {   // S103 PVR: a re-posed model (a ring piece / posed weapon) draws from this frame's geometry ring
      uint32_t ring_base = 0;
      if (PosedRingBase(fb, m, ring_base))
        op.vertex[0] = {rdr::kFrameGeometry, ring_base + me.vb_offset + (g_plant_posed_ring.load(std::memory_order_relaxed) == 1 ? uint32_t(sizeof(assets::Vertex)) : 0u)};
    }
    if ((me.cloth && !b.debug && !slots.empty()) || debug_posed) {
      // S101 lane 13: skinned cloth is posed here (the viewer skins it like any piece; the effect takes its position as it is, in the game's space)
      const uint64_t key = (uint64_t(it.object) << 32) | uint32_t(&me - m.meshes.data());
      rdr::BufferId& buf = m.cloth_bufs[key];
      uint32_t cloth_off = 0;   // S103 PVR: with g_posed_ring the posed piece goes into this frame's geometry ring (once per piece; the twin pass reuses the offset)
      bool in_ring = false;
      const auto ringed = cloth_ring.find(key);
      if (ringed != cloth_ring.end()) cloth_off = ringed->second, in_ring = true;
      else if (cloth_done.insert(key).second || buf == rdr::kNoBuffer) {
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
      if (g_posed_ring.load(std::memory_order_relaxed) && g_plant_posed_ring.load(std::memory_order_relaxed) == 3 && plant_cloth_prev_.count(key)) cloth_off = plant_cloth_prev_[key], cloth_ring[key] = cloth_off, in_ring = true;   // PLANT 3 (rig only): the previous Record's offset, nothing pushed
      else if (g_posed_ring.load(std::memory_order_relaxed) && PosedPush(fb, posed.data(), uint32_t(bytes), cloth_off)) cloth_ring[key] = cloth_off, in_ring = true, plant_cloth_prev_[key] = cloth_off;
      else if (buf == rdr::kNoBuffer) buf = renderer_.CreateBuffer(bytes, posed.data());
      else renderer_.UpdateBuffer(buf, 0, posed.data(), bytes, rdr::kBufCloth);
      }
      if (in_ring) op.vertex[0] = {rdr::kFrameGeometry, cloth_off + (g_plant_posed_ring.load(std::memory_order_relaxed) == 1 ? uint32_t(sizeof(assets::Vertex)) : 0u)};
      else op.vertex[0] = {buf, 0};
    }
    if (me.chunk) op.vertex[1] = {m.svb, me.sv_offset};  // S101 lane 13: palette slots + weights
    op.index = {m.ib, me.ib_offset};
    op.index_32bit = 1;
    op.count = me.index_count;
    op.instances = 1;
    if (!b.debug && oi.name_hash) {   // S103 P7: this draw is found again next tick by what it is (the object id's TEXT (review F1: Object::id is a blob offset), asset, mesh, placement instance, pass) + its vertex variant; the shader's interpolable constants are mixed
      uint64_t what = oi.name_hash ^ oi.asset;
      what = (what * 0x100000001b3ull) ^ uint32_t(&me - m.meshes.data());
      what = (what * 0x100000001b3ull) ^ (uint64_t(it.inst) << 8 | it.pass);
      TagOwnBlend(fb, op, vv, vs, vs_pushed, what);
    }
    if (FILE* fdf = fdump::File(); fdf && !b.debug) {   // S103 T2 (dev): what this draw is (feature_dump.h)
      int fsl[16]; uint32_t fim[16]; uint32_t fn = 0;
      for (uint32_t s = 0; s < fv.sampler_count && fn < 16; ++s) fsl[fn] = fv.samplers[s].slot, fim[fn++] = uint32_t(set.slots[fv.samplers[s].slot].image);
      for (uint32_t s = 0; s < vv.sampler_count && fn < 16; ++s) fsl[fn] = vv.samplers[s].slot, fim[fn++] = uint32_t(set.slots[vv.samplers[s].slot].image);  // S103 L10: the vertex stage's slots (16 + i)
      const auto fd_kind = [&](uint32_t im) -> const char* { return im == 0 ? "none" : im == uint32_t(white_) ? "white" : im == uint32_t(black_) ? "black" : im == uint32_t(grey_) ? "grey" : im == uint32_t(neutral_normal_) ? "neutral_normal" : im == uint32_t(black_cube_) ? "black_cube" : "map"; };
      fdump::Draw(fdf, "emit", mat.shader, me.part, b.chr, it.blend, vv.name, fv.name, T().by_index[size_t(vs)], T().by_index[size_t(ps)], fn, fsl, fim, fd_kind,
                  [&](const std::string& n, uint32_t v4, float* o) { return b.chr ? ResolveChar(n, v4, cc, o) : Resolve(n, v4, ctx, o); });
      fdump::StateLine(fdf, state, at_on != 0, at_on ? alpha_dyn(pcls) : dynamic, int(pcls), mat.diffuse_alpha_clear, int(fv.out_alpha));   // S103 B2 (dev); S105 SZW: at_on
    }
    if (mats_line_due) {   // S111 TOOLFIX3 (dev): the [mats] line, one per distinct material (the WWE_RDR_DUMP_MATS block above decides), from the FINAL draw state
      {
        const float* w = it.world.data();  // the sign of the 3x3 determinant: negative = a mirrored placement (the winding flips)
        const float det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) + w[2] * (w[4] * w[9] - w[5] * w[8]);
        const rdr::DynamicState md = at_on ? alpha_dyn(pcls) : dynamic;
        // at / aref255 / afunc = the draw's real alpha test (at_on; the reference x 255 and the VkCompareOp of its pass class); ben / bsrc / bdst = the blend the draw item gets (DrawState); mat_flag_atest = the LOADER's
        // material flag (Material::alpha_test: NOT the draw's test); list_blend / list_sorted = the draw-list flags of the item (it.blend 0 / 1 / 2, it.sorted): NOT the draw's blend state
        std::fprintf(stderr, "[mats] %-14s part=%-10s tex=%-14s vs=%s ps=%s spec_lev=%.2f pow=%.0f spec_col=%.1f light0=%.2f lm=%d at=%d aref255=%d afunc=%d ben=%d bsrc=%d bdst=%d cull=%d mat_flag_atest=%d list_blend=%d list_sorted=%d inl=%d ambw=%.2f difw=%.2f det=%s\n", mat.shader.c_str(), me.part.c_str(), mat.diffuse_name.c_str(),
                     kVariants[b.choice.vs].name, kVariants[b.choice.ps].name, mat.spec_lev, mat.spec_pow, mat.spec_col[0], ls.col[0][0], int(mat.lightmap >= 0), at_on ? 1 : 0, at_on ? int(md.system[8] * 255.f + 0.5f) : 0, at_on ? int(md.system[9]) : 7,
                     int(state.blend[0].enable), int(state.blend[0].src_color), int(state.blend[0].dst_color), int(state.cull), int(mat.alpha_test), int(it.blend), int(it.sorted), int(mat.blend_inline), mat.ambient[3], mat.diffuse_color[3], det < 0 ? "NEG" : "pos");
      }
    }
    if (fdump::File()) { const rdr::DynamicState d2_dyn = at_on ? alpha_dyn(pcls) : dynamic; d2(&state, &d2_dyn, &vv, &fv, &d2_sd); }   // S103 WGD (dev): the final draw state
    // S103 GSTP: this draw's group (a character mesh is a wrestler whatever its model says). The plant is read only while the stamps are on (TEST ONLY, host_rig --plant group_nolabel: the model draws are labelled `other`).
    const uint8_t group = fb.GroupsOn() && rdr::g_gpu_group_plant.load(std::memory_order_relaxed) == 3 ? uint8_t(rdr::kGrpOther) : is_char ? uint8_t(rdr::kGrpWrestler) : m.group;
    fb.Draw(op, group);
  };
  for (const Item& it : opaque) emit(it);
  run_stage(Stage::AfterOpaque);
  for (const Item& it : blended) emit(it);
  run_stage(Stage::AfterBlended);
  if (!in_seed_ && (szw_.game || szw_.ours) && !(szw_ == szw_logged_) && szw_lines_ < 12) {   // S105 SZW: the census line (proof 1 of SZW_contract.md): on a change, at most 12 a run
    ++szw_lines_, szw_logged_ = szw_;
    std::string names;
    for (const std::string& n : szw_.notwin_names) names += (names.empty() ? "" : ", ") + n;
    rdr::Log("[scene][szw] switch %s: stage-slot draws the game's rule gives a depth write %u, ours writes depth on %u, of which without a discard %u, game writers with no alpha twin %u%s%s; order items %u",
             zw_on ? "on" : "off", szw_.game, szw_.ours, szw_.nodisc, szw_.notwin, names.empty() ? "" : " (variants: ", names.empty() ? "" : (names + ")").c_str(), szw_.order_items);
  }
  if (!in_seed_ && hc_.draws && !(hc_ == hc_logged_) && hc_lines_ < 12) {   // S108 HAIR: the piece-cut census line, on a change, at most 12 a run
    ++hc_lines_, hc_logged_ = hc_;
    rdr::Log("[scene][hair_cut] wrestler piece translucent draws %u: alpha-tested GEQUAL 128/255 with depth write %u, no alpha twin (blended, no test, no depth write) %u, depth write without a discard %u",
             hc_.draws, hc_.cut, hc_.notwin, hc_.nodisc);
  }

  if (have_screen && live_screen_ != rdr::kNoImage) {  // S101 lane 257: this frame's scene colour becomes the next frame's screen picture
    rdr::CopyOp cp{};
    cp.source.image = tg.color, cp.dest.image = live_screen_;
    const BigScreenBox bx = BigScreenBoxOf(tg.width, tg.height);   // S105 A2: the centred 16:9 box (the whole target at 16:9)
    cp.source_rect = {int32_t(bx.x), int32_t(bx.y), bx.w, bx.h};
    cp.dest_rect = {0, 0, bx.w, bx.h};
    fb.Copy(cp);
    live_valid_ = true;
  }   // S103 SCX: a frame with no screen object keeps the last grab, as the game's texture does (SCG.md 3); only a scene / arena / size change clears it

  if (g_plant_flash_pre_post.load(std::memory_order_relaxed)) DrawFlashesAfterPost(frame, fb, tg.color, tg.depth, sctx);   // S110 CUTGREY planted twin (TEST ONLY): the old order, the flashes in the scene colour before the post chain
  run_stage(Stage::BeforePost);
  // S101 lane 34: the post pass (depth of field, haze, bloom, glare) on the scene's colour and depth
  if (frame.post) {
    if (!post_pass_) post_pass_ = std::make_unique<PostPass>(renderer_);
    PostParams pp = *frame.post;
    pp.width = tg.width, pp.height = tg.height;
    if (targets_scaled_ && tg.color == color_) pp.scaled_images = true;   // S105 A1: the chain (and its game-depth image) follows the targets actually drawn: the scene's own are scaled
    pp.depth_w = tg.width, pp.depth_h = tg.height;   // S103 SLV2: tg.depth is made with tg.color's size (EnsureTargets): the depth-convert fold's precondition (post_pass.h)
    if (FILE* fdf = fdump::File()) fdump::Pass(fdf, std::string("\"name\":\"post\",\"dof\":") + (pp.dof ? "1" : "0") + ",\"fog\":" + (pp.fog ? "1" : "0") + ",\"bloom\":" + (pp.bloom ? "1" : "0") + ",\"glare\":" + (pp.glare ? "1" : "0") + ",\"film\":" + (pp.film ? "1" : "0") + ",\"dead_bloom\":" + (pp.dead_bloom ? "1" : "0") + ",\"dead_glare\":" + (pp.dead_glare ? "1" : "0"));   // S103 T2 (dev)
    if (!post_pass_->Record(fb, tg.color, tg.depth, post_out_, pp, error)) return false;
  } else if (post_pass_ && !in_seed_) post_pass_->DisarmGhost(), post_pass_->DisarmLum();   // (S103 PST: the adapted luminance does not step on a no-post frame either)   // S103 FXF review 4: no post this frame = the game walk did not run with S+224 on: the afterimage ends (not the mirror seed frame: it is a no-post warm-up of the same guest frame)
  {   // S110 CUTGREY: the game draws the crowd flashes after its post chain (render category 52: after the combine and the sepia, before the fade and the vintage overlay, which the host draws on the final
      // target after this Record): onto the finished picture, crisp. `final_colour` = what the post chain wrote (the post output), or the scene colour when the frame has no post.
    const rdr::ImageId final_colour = frame.post ? post_out_ : tg.color;
    if (!g_plant_flash_pre_post.load(std::memory_order_relaxed)) flash_given_ += frame.flashes.size(), flash_drawn_ += DrawFlashesAfterPost(frame, fb, final_colour, tg.depth, sctx);   // S110 RDRPREP F3: given vs drawn
    sctx.color = final_colour;
    run_stage(Stage::AfterPost);
  }
  return true;
}

#include "front_end_flashes.h"   // S110 CUTGREY: FlashMeshesOf, DrawFlashesAfterPost (in this TU: FrontEnd::ModelData is private to this file)

// S103 lane D (FRAME_CONTRACT.md 5: a class's StageHook draws what the generic material path cannot: here instancing). One instanced draw per mesh of the model; the material is
// bound by name like any object's (BindMaterial), its yAudience vertex variant swapped for the instanced twin, the constants filled by name (Resolve) for an identity world.
uint32_t FrontEnd::DrawInstanced(const InstancedDraw& d, const StageCtx& sctx, rdr::FrameBuilder& fb, InstancedStats& st) {
  const auto refuse = [&](const std::string& why) { ++st.meshes_refused; if (st.first_refusal.empty()) st.first_refusal = why; };
  if (d.model < 0 || size_t(d.model) >= models_.size() || !models_[size_t(d.model)]->live || !d.posed || !d.worlds || d.count == 0) return 0;
  ModelData& m = Mod(d.model);
  ApplyLmRow(m);   // S103 B3: the crowd takes the frame's row too (SetLmRow ran at this frame's Record)
  const float* vp = d.view_proj ? d.view_proj : sctx.view_proj;
  const float* eye = d.eye ? d.eye : sctx.eye;
  if (!vp || !eye) return 0;
  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(sctx.width);
  dynamic.viewport[3] = float(sctx.height);
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = sctx.width;
  dynamic.scissor.height = sctx.height;
  if (d.viewport_rect) {   // S103 CB1: one tile of a shared target (the card atlas)
    dynamic.viewport[0] = float(d.viewport_rect->x), dynamic.viewport[1] = float(d.viewport_rect->y);
    dynamic.viewport[2] = float(d.viewport_rect->width), dynamic.viewport[3] = float(d.viewport_rect->height);
    dynamic.scissor = d.scissor_rect ? *d.scissor_rect : *d.viewport_rect;
  }
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;
  dynamic.system[9] = 7.f;
  const bool dry = d.dry && d.input_hash;   // S103 CB1: hash only, record nothing
  const auto mix = [&](const void* p, size_t n) { if (d.input_hash) *d.input_hash = AtlasMix(*d.input_hash, p, uint32_t(n)); };
  const assets::LightSet default_rig = assets::DefaultRig();
  const assets::Matrix identity = assets::Identity();
  uint32_t instance_offset = 0;
  bool have_instances = false;
  std::vector<uint8_t>& block = inst_block_;  // (kept across frames: no per-frame allocation)
  uint32_t draws = 0;
  for (const ModelData::Mesh& me : m.meshes) {
    if (me.chunk || me.skinned || me.placement >= 0) { refuse("mesh is skinned / placed: not an instanced crowd piece"); continue; }
    if (size_t(me.src) >= d.posed->size() || (*d.posed)[size_t(me.src)].size() != me.vertex_count) { refuse("posed vertices missing or of another count for mesh " + std::to_string(me.src)); continue; }
    const assets::Material fallback;
    const assets::Material& mat = me.material >= 0 ? m.materials[size_t(me.material)] : fallback;
    const Bound& b = BindMaterial(m, me.material, false, LmMode::Own);
    if (b.debug || b.chr || !b.choice.Ok()) { refuse("material '" + mat.shader + "' has no effect: " + b.choice.why_not); continue; }
    const int vs_twin = InstTwinOf(b.choice.vs);
    if (vs_twin < 0) { refuse(std::string("no instanced twin of ") + kVariants[b.choice.vs].name); continue; }
    const uint16_t layout = InstLayoutFor(b.choice.vs);  // (the PLAIN variant's input table: the twin has none of its own; its block layout is the plain one's, proved in InstTwinOf)
    if (layout == 0xFFFF) { refuse(std::string("instanced twin ") + kVariants[vs_twin].name + " reads an input the instanced layout does not carry"); continue; }
    const int ps = b.choice.ps;
    const int set_index = me.light_set;
    // S103 L4: the frame's own sets (d.lights, the record's values by id) replace the file's baseline: the crowd's set 20 is 0.4706 in the file and 0.396 (other directions, sky, ground) in the game's draws
    const bool lights_fit = d.lights && d.lights->sets.size() == m.light_sets.size();
    if (d.lights && !lights_fit) ++st.light_mismatch;   // a set-count mismatch falls back to the loader's sets: counted (LiveCrowdCounts::light_fallbacks), never silent
    const std::vector<assets::LightSet>& use_sets = lights_fit ? d.lights->sets : m.light_sets;
    const assets::LightSet& ls = (set_index >= 0 && size_t(set_index) < use_sets.size()) ? use_sets[size_t(set_index)] : default_rig;
    Ctx ctx{identity.data(), vp, eye, 0.0, &mat, &ls, m.lm_xy, m.lm_gain, 1.f};
    ctx.game_space = d.game_space;
    if (b.cube_real) ctx.reflect_alpha = mat.reflect_alpha;   // S103 RFL (the instanced crowd: same rule as the plain draw)
    ctx.mirror = (d.game_space || options_.plant_no_mirror) ? 1.f : -1.f;
    static const bool dump_crowd_light = std::getenv("WWE_RDR_DUMP_CROWD_LIGHT") != nullptr;   // (read once: this loop runs per mesh per frame)
    if (dump_crowd_light) {   // S103 L4 (dev, tools\scene\l4_check.py): the set this instanced draw is lit with, once per model
      static std::set<int> said;
      if (said.insert(d.model).second)
        std::fprintf(stderr, "[crowd-var] model %d vs %s ps %s\n", d.model, kVariants[b.choice.vs].name, kVariants[ps].name);
      if (said.insert(d.model + 100000).second)
        std::fprintf(stderr, "[crowd-lit] model %d set_idx %d id %d col=%.5f %.5f %.5f|%.5f %.5f %.5f|%.5f %.5f %.5f|%.5f %.5f %.5f sky=%.5f %.5f %.5f gnd=%.5f %.5f %.5f dir0=%.5f %.5f %.5f dir3=%.5f %.5f %.5f\n", d.model, set_index, ls.id,
                     ls.col[0][0], ls.col[0][1], ls.col[0][2], ls.col[1][0], ls.col[1][1], ls.col[1][2], ls.col[2][0], ls.col[2][1], ls.col[2][2], ls.col[3][0], ls.col[3][1], ls.col[3][2], ls.sky[0], ls.sky[1], ls.sky[2],
                     ls.ground[0], ls.ground[1], ls.ground[2], ls.dir[0][0], ls.dir[0][1], ls.dir[0][2], ls.dir[3][0], ls.dir[3][1], ls.dir[3][2]);
    }
    uint32_t pushed_bytes = 0;   // S103 P7 (review F8): the size of the block pack() pushed last
    const auto pack = [&](int variant, const rdr::fx::Variant& v) {
      const fxt::VariantTable* t = T().by_index[size_t(variant)];
      const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(v.block_bytes, 16), 8192);
      pushed_bytes = bytes;
      block.assign(bytes, 0);
      if (t)
        for (uint32_t k = 0; k < t->field_count; ++k) {
          const fxt::Field& f = t->fields[k];
          if (f.offset + f.vec4s * 16u > bytes) continue;
          if (!Resolve(f.name, f.vec4s, ctx, reinterpret_cast<float*>(block.data() + f.offset))) missing_fields_.insert(f.name);
        }
      mix(block.data(), bytes);   // S103 CB1
      if (dry) return uint32_t(0);
      return fb.PushConstants(block.data(), bytes);
    };
    const rdr::fx::Variant& fv = kVariants[ps];
    // S111 CROWDATEST: the caller's test when it names one (InstancedDraw::alpha_test: the game's crowd node decides per draw family), else the material's (program by its flag, state by the bind's)
    const bool prog_tested = d.alpha_test < 0 ? mat.alpha_test : d.alpha_test != 0;
    const bool tested = d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0;
    rdr::DrawState state{};
    state.program = ProgramFor(vs_twin, ps, prog_tested);
    state.vertex_layout = layout;
    state.topology = 3;
    state.cull = d.plant_two_sided ? 0 : 2;   // S111 CROWDCULLSRC: back faces culled on every instanced draw, as the game: each is a crowd draw under the crowd node's cull-on (sub_82519508 audience_draw_entry +178), the card bake too (docs\systems\crowd.md "Far-card cull"); the material's record flag is the generic model draw's rule, not this one's
    state.front_face = 1;
    state.depth_test = 1;
    state.depth_write = 1;
    const pass_state::PassClass pcls = pass_state::ClassOf(mat.shader.c_str(), false);   // S103 B2: by effect name, as the generic path (the crowd cards are yAudience*)
    state.depth_compare = pass_state::ForwardZ(pass_state::Of(pcls).game_z_func);
    state.blend[0].write_mask = 0xF;
    if (tested && pass_state::Of(pcls).tested_blends && !options_.plant_cutout_noblend) {   // S103 B2 (review 1): a TESTED part (the crowd card, GEQUAL 192) is blended like the game's [C census]; the z write stays
      state.blend[0].enable = 1;
      state.blend[0].src_color = 6, state.blend[0].dst_color = 7, state.blend[0].src_alpha = 1, state.blend[0].dst_alpha = 7;
    }
    rdr::TextureSet set{};
    for (uint32_t s = 0; s < fv.sampler_count; ++s) {
      const int slot = fv.samplers[s].slot;
      rdr::SamplerDesc sd{};
      sd.mag_filter = sd.min_filter = 1;
      sd.mip_mode = 1;
      sd.address_u = sd.address_v = sd.address_w = (b.slot_clamp[slot] || b.cube_slot[slot] || (slot == 0 && d.clamp_slot0)) ? 2 : 0;
      sd.max_anisotropy = sampler_table::AnisoWanted();   // S103 B1
      sd.max_lod = 1000.f;
      TableSampler(b.samp, b.choice, slot, b.cube_slot[slot] == 4 || b.cube_slot[slot] == 5, false, sd);   // S103 B1
      if (slot == 0 && d.clamp_slot0) sd.address_u = sd.address_v = sd.address_w = 2;   // the crowd's own clamp stays (not a game sampler value we decode)
      if (slot == 0 && d.aniso_override) sd.max_anisotropy = d.aniso_override;   // S103 CB1
      set.slots[slot].image = (slot == 0 && d.slot0 != rdr::kNoImage) ? d.slot0 : b.slot_image[slot];
      if (dry) mix(&sd, sizeof sd);   // S103 CB1
      set.slots[slot].sampler = dry ? 0u : fb.Sampler(sd);
      set.slots[slot].kind = fv.samplers[s].kind;
    }
    for (uint32_t s = 0; b.vtx_lm && s < kVariants[b.choice.vs].sampler_count; ++s) {  // S103 L10: the crowd's page in the vertex stage (slot 16)
      const int slot = kVariants[b.choice.vs].samplers[s].slot;
      rdr::SamplerDesc sd{};
      sd.mag_filter = sd.min_filter = 1;
      sd.mip_mode = 1;
      sd.address_u = sd.address_v = sd.address_w = 2;
      sd.max_lod = 1000.f;
      TableSampler(b.samp, b.choice, slot, false, true, sd);   // S103 B1
      set.slots[slot].image = b.slot_image[slot];
      if (dry) mix(&sd, sizeof sd);   // S103 CB1
      set.slots[slot].sampler = dry ? 0u : fb.Sampler(sd);
      set.slots[slot].kind = kVariants[b.choice.vs].samplers[s].kind;
      set.slots[slot].vertex = 1;
    }
    if (dry) {   // S103 CB1: the inputs of this mesh's draw, hashed; nothing pushed or recorded
      const std::vector<InstVertex>& hv = (*d.posed)[size_t(me.src)];
      const rdr::DynamicState hd = tested ? AlphaDynamic(dynamic, pcls, options_.plant_old_alpha_ref) : dynamic;
      mix(&state, sizeof state), mix(&hd, sizeof hd);
      for (const rdr::TextureSlot& ts : set.slots) mix(&ts.image, sizeof ts.image), mix(&ts.kind, sizeof ts.kind), mix(&ts.vertex, sizeof ts.vertex);
      const int hm = d.model;
      mix(&hm, sizeof hm), mix(&me.src, sizeof me.src);
      pack(b.choice.vs, kVariants[b.choice.vs]), pack(ps, fv);
      mix(hv.data(), hv.size() * sizeof(InstVertex)), mix(d.worlds, size_t(d.count) * 48u);
      ++draws;
      continue;
    }
    if (!have_instances) instance_offset = fb.PushGeometry(d.worlds, d.count * 48u, 16), have_instances = true;
    const std::vector<InstVertex>& verts = (*d.posed)[size_t(me.src)];
    rdr::DrawOp op{};
    op.state = fb.State(state);
    op.dynamic = fb.Dynamic(tested ? AlphaDynamic(dynamic, pcls, options_.plant_old_alpha_ref) : dynamic);
    op.textures = fb.Textures(set);
    op.vs_constants = pack(b.choice.vs, kVariants[b.choice.vs]);
    const uint32_t vs_pushed = pushed_bytes;
    op.fs_constants = pack(ps, fv);
    op.vertex[0] = {rdr::kFrameGeometry, fb.PushGeometry(verts.data(), uint32_t(verts.size() * sizeof(InstVertex)), 16)};
    op.vertex[1] = {rdr::kFrameGeometry, instance_offset};
    op.index = {m.ib, me.ib_offset};
    op.index_32bit = 1;
    op.count = me.index_count;
    op.instances = d.count;
    const bool tag_this = fb.BlendTagsOn() && (!d.no_blend || g_own_blend_plant.load(std::memory_order_relaxed) == 2);
    if (fb.BlendTagsOn() && d.no_blend) { g_own_blend_census.bake_seen.fetch_add(1, std::memory_order_relaxed); if (tag_this) g_own_blend_census.bake_tagged.fetch_add(1, std::memory_order_relaxed); }
    if (tag_this) {
      if (!d.no_blend) g_own_blend_census.instanced_tagged.fetch_add(1, std::memory_order_relaxed);   // S103 P7: the instanced crowd: found again by model + mesh (its camera constants follow the tick blend; the instances' own worlds are inline geometry and stay this tick's). A card BAKE (no_blend, review F2) is never tagged: it shares model + mesh with the LOD-0 members
      TagOwnBlend(fb, op, kVariants[b.choice.vs], b.choice.vs, vs_pushed, ((uint64_t(uint32_t(d.model)) << 32 | uint32_t(me.src)) ^ 0xC20D) + uint64_t(uint32_t(d.slot0)) * 0x9E3779B97F4A7C15ull + uint64_t(d.tag_salt) * 0xD6E8FEB86659FD93ull);   // (+ the slot-0 picture: the card types and the prop groups share a quad / model)
    }
    if (FILE* fdf = fdump::File()) {   // S103 T2 (dev): the instanced crowd draw
      int fsl[16]; uint32_t fim[16]; uint32_t fn = 0;
      for (uint32_t s = 0; s < fv.sampler_count && fn < 16; ++s) fsl[fn] = fv.samplers[s].slot, fim[fn++] = uint32_t(set.slots[fv.samplers[s].slot].image);
      for (uint32_t s = 0; s < kVariants[vs_twin].sampler_count && fn < 16; ++s) fsl[fn] = kVariants[vs_twin].samplers[s].slot, fim[fn++] = uint32_t(set.slots[kVariants[vs_twin].samplers[s].slot].image);  // S103 L10: the vertex stage's slots
      const auto fd_kind = [&](uint32_t im) -> const char* { return im == 0 ? "none" : im == uint32_t(white_) ? "white" : im == uint32_t(black_) ? "black" : im == uint32_t(grey_) ? "grey" : im == uint32_t(neutral_normal_) ? "neutral_normal" : im == uint32_t(black_cube_) ? "black_cube" : "map"; };
      fdump::Draw(fdf, "instanced", mat.shader, me.part, false, 0, kVariants[vs_twin].name, fv.name, T().by_index[size_t(b.choice.vs)], T().by_index[size_t(ps)], fn, fsl, fim, fd_kind,
                  [&](const std::string& n, uint32_t v4, float* o) { return Resolve(n, v4, ctx, o); });
      fdump::StateLine(fdf, state, tested, tested ? AlphaDynamic(dynamic, pcls, options_.plant_old_alpha_ref) : dynamic, int(pcls), mat.diffuse_alpha_clear, int(fv.out_alpha));   // S103 B2 (dev)
    }
    fb.Draw(op, d.group);   // S103 GSTP: the caller's group (InstancedDraw::group)
    EffectCoverage& cov = coverage_[mat.shader];
    cov.effect = mat.shader;
    ++cov.meshes_bound;
    ++draws;
  }
  st.draws += draws;
  st.instances += draws * d.count;
  return draws;
}

// S103 D2: the first reason DrawInstanced would refuse a mesh of this model (the checks of the loop above that do not depend on the frame), "" = none. Asked at Build time, so a class that can never
// be drawn is a counted hole before any frame is recorded.
std::string FrontEnd::InstancedRefusal(int model) {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return "no such model";
  ModelData& m = Mod(model);
  for (const ModelData::Mesh& me : m.meshes) {
    if (me.chunk || me.skinned || me.placement >= 0) return "mesh is skinned / placed: not an instanced crowd piece";
    const assets::Material fallback;
    const assets::Material& mat = me.material >= 0 ? m.materials[size_t(me.material)] : fallback;
    const Bound& b = BindMaterial(m, me.material, false, LmMode::Own);
    if (b.debug || b.chr || !b.choice.Ok()) return "material '" + mat.shader + "' has no effect: " + b.choice.why_not;
    const int vs_twin = InstTwinOf(b.choice.vs);
    if (vs_twin < 0) return std::string("no instanced twin of ") + kVariants[b.choice.vs].name;
    if (InstLayoutFor(b.choice.vs) == 0xFFFF) return std::string("instanced twin ") + kVariants[vs_twin].name + " reads an input the instanced layout does not carry";
  }
  return std::string();
}

// S101 lane 15: a live texture (the entrance's tron video frames, the stage light-map page) takes a new picture. The caller gives RGBA8 of the
// texture's own size; the GPU is idle between frames (RenderToPixels waits).
uint64_t FrontEnd::TextureImage(int model, size_t texture) const {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return 0;
  const ModelData& m = *models_[size_t(model)];
  return texture < m.images.size() && m.images[texture] != rdr::kNoImage ? uint64_t(m.images[texture]) : 0;
}

bool FrontEnd::UpdateTexture(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba, TexTag tag) {
  if (tag == TexTag::Screen && g_screen_share.load(std::memory_order_relaxed) && !g_plant_screen_reupload) {   // S103 UPL U2: a screen picture goes through the shared pool (a batch of one)
    uint64_t id = 1469598103934665603ull;   // (a lone caller has no feed id: the id is a hash of the bytes, top bit set so it never meets a feed's id)
    for (size_t i = 0, n = rgba ? size_t(w) * h * 4 : 0; i < n; ++i) id = (id ^ rgba[i]) * 1099511628211ull;
    const ScreenPic p{texture, w, h, rgba, id | (1ull << 63)};
    bool ok = false;
    UpdateScreens(model, &p, 1, &ok);
    return ok;
  }
  return UpdateTextureDirect(model, texture, w, h, rgba, tag);
}

bool FrontEnd::UpdateTextureDirect(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba, TexTag tag) {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  if (texture >= m.images.size() || m.images[texture] == rdr::kNoImage || !rgba || !w || !h) return false;
  const ModelData::ImgInfo& ii = m.img_info[texture];
  if (!ii.rgba8 || ii.mips != 1 || ii.w != w || ii.h != h) return false;  // only a 1-mip RGBA8 image of exactly this size (a live texture) is replaced
  if (texture < m.scr_of.size() && m.scr_of[texture] >= 0) {   // S103 UPL: a pooled screen image: written only when no other screen shows it (else this screen gets its own), and what it holds is no longer a known picture
    const int e = ScrPrivateEntry(m, model, texture);
    if (e < 0) return false;
    m.scr_pool[size_t(e)].known = false, m.scr_pool[size_t(e)].id = 0;
  }
  renderer_.UpdateImage(m.images[texture], 0, 0, rgba, size_t(w) * h * 4);
  ++texture_updates_;
  tex_bytes_[size_t(tag)] += uint64_t(w) * h * 4, ++tex_count_[size_t(tag)];   // S103 UP
  return true;
}

// S103 UPL: a model's texture-slot image changed: the bound materials hold the old id. ONE place for the trio (every `.images[..] =` in this file is followed by it: tools\scene\upl_lint.py).
void FrontEnd::ResetModelBindings(ModelData& m, int model) {
  for (auto& b : m.bound) b.reset();
  m.char_bound.clear();   // S103 YCB
  m.rule_bound.clear(), m.rule_pick.clear();
  if (model >= 0 && draw_lists_) draw_lists_->ReleaseAsset(uint32_t(model));
}

// The pool entry only `texture` shows (S103 UPL, the direct write of UpdateTextureDirect): its own when nobody else shows it, else a free same-size one, else a new image.
int FrontEnd::ScrPrivateEntry(ModelData& m, int model, size_t texture) {
  auto& pool = m.scr_pool;
  const int cur = m.scr_of[texture];
  if (pool[size_t(cur)].users == 1) return cur;
  int tgt = -1;
  for (size_t e = 0; e < pool.size() && tgt < 0; ++e)
    if (pool[e].users == 0 && pool[e].image != rdr::kNoImage && pool[e].w == pool[size_t(cur)].w && pool[e].h == pool[size_t(cur)].h) tgt = int(e);
  if (tgt < 0) {
    rdr::ImageDesc d;
    d.width = uint16_t(pool[size_t(cur)].w), d.height = uint16_t(pool[size_t(cur)].h), d.mips = 1, d.usage = rdr::kUsageSampled, d.format = rdr::Format::RGBA8;
    ModelData::ScrImg img;
    img.image = renderer_.CreateImage(d), img.w = pool[size_t(cur)].w, img.h = pool[size_t(cur)].h;
    if (img.image == rdr::kNoImage) return -1;
    pool.push_back(std::move(img));
    tgt = int(pool.size() - 1);
    ++scr_images_;
  }
  --pool[size_t(cur)].users, ++pool[size_t(tgt)].users;
  m.scr_of[texture] = tgt;
  m.images[texture] = pool[size_t(tgt)].image;
  ResetModelBindings(m, model);
  ++scr_moved_;
  return tgt;
}

// S103 UPL U2 (docs\renderer\360_HABITS.md row 8): the game's screens each have their own surface and each is written per frame (a per-surface resolve into unified memory, free on the 360). We upload each DISTINCT
// picture once (pictures are told apart by the feed's content id, never by pointer or by comparing bytes) and bind every screen that shows it to the same image. The pictures are equal bytes, so the draw is unchanged;
// no screen's picture is ever overwritten while it is still shown. A screen that was just re-bound is not re-bound again for a one-frame serial lag (kMoveHold batches): it writes its own image instead.
bool FrontEnd::UpdateScreens(int model, const ScreenPic* pics, size_t n, bool* ok) {
  for (size_t i = 0; i < n; ++i) ok[i] = false;
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  if (!g_screen_share.load(std::memory_order_relaxed) || g_plant_screen_reupload || m.images_shared || m.images_external) {
    bool any = false;
    for (size_t i = 0; i < n; ++i) any = (ok[i] = UpdateTextureDirect(model, pics[i].texture, pics[i].w, pics[i].h, pics[i].rgba, TexTag::Screen)) || any;
    return any;
  }
  constexpr int64_t kMoveHold = 8;
  auto& pool = m.scr_pool;
  if (m.scr_of.size() < m.images.size()) m.scr_of.resize(m.images.size(), -1);
  if (m.scr_moved_at.size() < m.images.size()) m.scr_moved_at.resize(m.images.size(), -1000);
  ++scr_batch_no_;
  std::vector<size_t> todo;
  for (size_t i = 0; i < n; ++i) {
    const ScreenPic& p = pics[i];
    if (p.texture >= m.images.size() || m.images[p.texture] == rdr::kNoImage || !p.id || !p.w || !p.h) continue;
    const ModelData::ImgInfo& ii = m.img_info[p.texture];
    if (!ii.rgba8 || ii.mips != 1 || ii.w != p.w || ii.h != p.h) continue;   // only a 1-mip RGBA8 image of exactly this size (a live texture) is replaced
    int& e = m.scr_of[p.texture];
    if (e >= 0 && pool[size_t(e)].image != m.images[p.texture]) {   // the texture's image was changed behind the pool (not ours to keep or destroy): the old entry leaves the pool, dead
      ModelData::ScrImg& old = pool[size_t(e)];
      --old.users;
      if (old.users <= 0) old.image = rdr::kNoImage, old.known = false, old.w = old.h = 0, --scr_images_;
      e = -1;
    }
    if (e < 0) {   // the texture's own image joins the pool (its contents are unknown: it never matches a picture)
      ModelData::ScrImg img;
      img.image = m.images[p.texture], img.w = ii.w, img.h = ii.h, img.users = 1;
      pool.push_back(std::move(img));
      e = int(pool.size() - 1);
      ++scr_images_;
    }
    todo.push_back(i);
  }
  std::vector<char> done(n, 0);
  bool rebind = false, took = false;
  for (size_t a : todo) {
    if (done[a]) continue;
    const ScreenPic& lead = pics[a];
    const size_t bytes = size_t(lead.w) * lead.h * 4;
    std::vector<size_t> group{a};   // the pictures of this batch with the same content id and size: one image for all
    for (size_t b : todo)
      if (b != a && !done[b] && pics[b].id == lead.id && pics[b].w == lead.w && pics[b].h == lead.h) group.push_back(b);
    for (size_t g : group) done[g] = 1;
    const uint8_t* rgba = nullptr;
    for (size_t g : group)
      if (pics[g].rgba) { rgba = pics[g].rgba; break; }
    int tgt = -1;
    for (size_t e = 0; e < pool.size() && tgt < 0; ++e)   // 1. an image that already holds this picture: no upload
      if (pool[e].image != rdr::kNoImage && pool[e].known && pool[e].id == lead.id && pool[e].w == lead.w && pool[e].h == lead.h) tgt = int(e);
    bool upload = false;
    if (tgt < 0) {
      if (!rgba) continue;   // (no bytes and no image holds it: not taken)
      upload = true;
      for (size_t g : group) {   // 2. an image shown only by this group's screens: written in place (nobody else loses a picture)
        const int e = m.scr_of[pics[g].texture];
        int on = 0;
        for (size_t h : group) on += m.scr_of[pics[h].texture] == e;
        if (pool[size_t(e)].w == lead.w && pool[size_t(e)].h == lead.h && pool[size_t(e)].users == on) { tgt = e; break; }
      }
      for (size_t e = 0; e < pool.size() && tgt < 0; ++e)   // 3. a free image of this size
        if (pool[e].users == 0 && pool[e].image != rdr::kNoImage && pool[e].w == lead.w && pool[e].h == lead.h) tgt = int(e);
      if (tgt < 0) {   // 4. a new image (the screens keep their pictures while this one is made)
        rdr::ImageDesc d;
        d.width = uint16_t(lead.w), d.height = uint16_t(lead.h), d.mips = 1, d.usage = rdr::kUsageSampled, d.format = rdr::Format::RGBA8;
        ModelData::ScrImg img;
        img.image = renderer_.CreateImage(d), img.w = lead.w, img.h = lead.h;
        if (img.image == rdr::kNoImage) continue;   // (not taken: ok stays false)
        pool.push_back(std::move(img));
        tgt = int(pool.size() - 1);
        ++scr_images_;
      }
    }
    const auto send = [&](int e) {
      ModelData::ScrImg& t = pool[size_t(e)];
      renderer_.UpdateImage(t.image, 0, 0, rgba, bytes);
      ++texture_updates_;
      tex_bytes_[size_t(TexTag::Screen)] += bytes, ++tex_count_[size_t(TexTag::Screen)];
      t.known = true, t.id = lead.id;
    };
    if (upload) send(tgt);
    std::vector<int> wrote;   // entries a screen wrote in place instead of moving (the hold)
    for (size_t g : group) {
      const size_t tex = pics[g].texture;
      const int e = m.scr_of[tex];
      if (e != tgt) {
        int on = 0;
        for (size_t h : group) on += m.scr_of[pics[h].texture] == e;
        const bool writable = pool[size_t(e)].users == on && pool[size_t(e)].w == lead.w && pool[size_t(e)].h == lead.h;
        const bool recent = m.scr_moved_at[tex] >= 0 && int64_t(scr_batch_no_) - m.scr_moved_at[tex] < kMoveHold;
        if (writable && rgba && recent) {   // just moved: stay on the own image, write it (once for the screens on it)
          if (std::find(wrote.begin(), wrote.end(), e) == wrote.end()) send(e), wrote.push_back(e);
          ok[g] = true, took = true;
          continue;
        }
        --pool[size_t(e)].users, ++pool[size_t(tgt)].users;
        m.scr_of[tex] = tgt;
        m.images[tex] = pool[size_t(tgt)].image;   // rebind-after: ResetModelBindings below, once for the batch
        m.scr_moved_at[tex] = int64_t(scr_batch_no_);
        rebind = true, ++scr_moved_;
      }
      ok[g] = true, took = true;
    }
    scr_pics_ += group.size();
    const size_t sent = (upload ? 1 : 0) + wrote.size();   // uploads this group made: the rest of its pictures cost nothing
    scr_shared_ += group.size() > sent ? group.size() - sent : 0;
  }
  if (rebind) ResetModelBindings(m, model);   // a bound material holds the image id it was bound with: rebuilt
  if (took) ++scr_batches_;
  if (rebind) ++scr_rebind_batches_;
  return took;
}

bool FrontEnd::AdoptPage(int model, size_t texture, uint64_t group) {
  if (g_plant_page_per_model || group == 0) return false;
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  if (m.page_group == group && m.page_tex >= 0 && size_t(m.page_tex) == texture) return true;
  if (m.page_group != 0) return false;   // JoinCurrentPage leaves the old group first (LeavePage)
  const bool borrowed = m.images_external || m.images_shared;   // the images belong to another owner (the crowd): never destroyed here
  if (texture >= m.images.size() || m.images[texture] == rdr::kNoImage) return false;
  const ModelData::ImgInfo& ii = m.img_info[texture];
  if (!ii.rgba8 || ii.mips != 1) return false;
  auto it = page_shares_.find(group);
  if (it == page_shares_.end()) {
    if (borrowed) return false;   // the first adopter's image becomes the group's: it must be this model's own
    PageShare s;
    s.image = m.images[texture], s.refs = 1, s.w = ii.w, s.h = ii.h;   // holds what the model was built with = key 0 (the match page)
    page_shares_[group] = std::move(s);
    m.page_orig = rdr::kNoImage;
  } else {
    PageShare& s = it->second;
    if (s.w != ii.w || s.h != ii.h) return false;
    m.page_orig = borrowed ? m.images[texture] : rdr::kNoImage;   // a borrowed model's own image comes back when it leaves the group
    if (!borrowed && m.images[texture] != s.image) renderer_.DestroyImage(m.images[texture]);   // the private copy goes (deferred by the renderer); an external image stays with its owner
    m.images[texture] = s.image;
    ++s.refs;
  }
  m.page_tex = int(texture), m.page_group = group;
  ResetModelBindings(m, model);   // a bound material holds the old image id
  return true;
}

void FrontEnd::LeavePage(ModelData& m, bool restore) {
  if (m.page_group != 0 && m.page_tex >= 0 && size_t(m.page_tex) < m.images.size()) {
    const auto it = page_shares_.find(m.page_group);
    if (it != page_shares_.end() && --it->second.refs <= 0) {   // the last reference: the group's image goes with it
      if (it->second.image != rdr::kNoImage) renderer_.DestroyImage(it->second.image);
      page_shares_.erase(it);
    }
    m.images[size_t(m.page_tex)] = restore ? m.page_orig : rdr::kNoImage;
    if (restore) ResetModelBindings(m, -1);   // the model's materials hold the group image id: rebuild them (the caller releases the kept draw lists)
  }
  m.page_group = 0, m.page_tex = -1, m.page_orig = rdr::kNoImage;
}

bool FrontEnd::JoinCurrentPage(int model, size_t texture) {
  if (page_group_current_ == 0 || model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  const bool borrowed = m.images_external || m.images_shared;
  if (m.page_group == page_group_current_) return true;
  if (m.page_group != 0) {   // a model of a RETIRED set's group (a rebuilt set: the crowd keeps its models): leave it, join the current one (one page image, one upload)
    if (g_plant_page_no_rejoin) return true;   // the old fault: it stays where it joined first
    if (!borrowed || m.page_tex < 0 || size_t(m.page_tex) != texture) return false;
    LeavePage(m, true);
    if (draw_lists_) draw_lists_->ReleaseAsset(uint32_t(model));
  } else if (!borrowed) {
    return false;
  }
  return AdoptPage(model, texture, page_group_current_);
}

uint64_t FrontEnd::PageGroupOf(int model) const {
  return model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live ? 0 : models_[size_t(model)]->page_group;
}

uint64_t FrontEnd::PageKeyOf(int model, size_t texture) const {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return ~uint64_t(0);
  const ModelData& m = *models_[size_t(model)];
  if (m.page_group == 0 || m.page_tex < 0 || size_t(m.page_tex) != texture) return ~uint64_t(0);
  const auto it = page_shares_.find(m.page_group);
  return it == page_shares_.end() ? ~uint64_t(0) : it->second.key;
}

bool FrontEnd::UpdatePage(int model, size_t texture, uint64_t key, uint32_t w, uint32_t h, const uint8_t* rgba) {
  if (model < 0 || size_t(model) >= models_.size() || !models_[size_t(model)]->live) return false;
  ModelData& m = *models_[size_t(model)];
  JoinCurrentPage(model, texture);   // a crowd model made after the arena's page (or left in a retired set's group) joins the current one
  if (m.page_group == 0 || m.page_tex < 0 || size_t(m.page_tex) != texture) return UpdateTexture(model, texture, w, h, rgba, TexTag::Page);
  const auto it = page_shares_.find(m.page_group);
  if (it == page_shares_.end() || !rgba || !w || !h || it->second.w != w || it->second.h != h) return false;
  PageShare& s = it->second;
  const size_t n = size_t(w) * h * 4;
  const bool per_caller = rdr_sw::BoolOr(rdr_sw::kPagePerCaller, g_rig_page_per_caller);   // +dev switch: every caller sends (the old byte cost; an in-run A/B on the Surface)
  if (!per_caller) {
    if (s.key == key) { ++page_skip_key_; return true; }   // the group's one image already holds this page
    if (s.sent.size() == n && std::memcmp(s.sent.data(), rgba, n) == 0) { s.key = key; ++page_skip_hash_; return true; }   // new key, same bytes (exact, vectorised memcmp: no hash)
  }
  renderer_.UpdateImage(s.image, 0, 0, rgba, n);
  ++texture_updates_;
  tex_bytes_[size_t(TexTag::Page)] += n, ++tex_count_[size_t(TexTag::Page)];
  s.key = key;
  if (!per_caller) s.sent.assign(rgba, rgba + n);
  else s.sent.clear();
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
  if (draw_lists_) draw_lists_->Clear();  // S103 G: bindings are made again
  for (ModelData* m : models_) {
    for (auto& b : m->bound) b.reset();  // re-bind (the coverage is filled while binding)
    m->char_bound.clear();   // S103 YCB
    m->rule_bound.clear(), m->rule_pick.clear();   // review 4 (S103 D1)
  }
}

}  // namespace rdr_scene
