// See arena_game_space.h. Session 101, lane 40.
#include "arena_game_space.h"

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like crowd_body.cpp (the game exe and the phone library must
// not export stbi_*; pac.h / stage_media.h use stb). Only these two files include asset_load.h in the game build.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include "asset_load.h"
#include "prop_host_vertex.h"   // S101 lane 49: the skinned prop vertex words (shared with the ring)

namespace assets::arena_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;
}  // namespace

size_t StageArena::Bytes() const {
  size_t n = 0;
  for (const StageMesh& m : meshes) n += m.vertices.size() * sizeof(Vertex) + m.indices.size() * 4 + m.skin_ids.size() + m.skin_weights.size();   // (+ the skin words, S101 lane 49)
  for (const Texture& t : textures) n += t.data.size();
  return n;
}

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(int arena, StageArena& out, std::string& err, bool viewer_space) {
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // S101 lane 40: one loader at a time (asset_load.h is not thread-safe; the crowd and the arena load on workers)
  if (!g_ready) return err = "arena_game_space: no game files (Init first)", false;
  out = StageArena{};
  out.arena = arena;
  Model model;
  const auto saved = load_detail::CrowdPose();
  load_detail::CrowdPose() = load_detail::CrowdPoseMode::kStored;  // the crowd's pose is not read here: the cheap mode
  const bool ok = LoadArena(g_files, arena, model, err);
  load_detail::CrowdPose() = saved;
  if (!ok) return false;
  std::map<int, int> remap;  // model texture -> out.textures
  auto map_tex = [&](int idx) -> int {
    if (idx < 0 || size_t(idx) >= model.textures.size()) return -1;
    auto it = remap.find(idx);
    if (it == remap.end()) {
      out.textures.push_back(model.textures[size_t(idx)]);
      it = remap.emplace(idx, int(out.textures.size()) - 1).first;
    }
    return it->second;
  };
  for (const Mesh& me : model.meshes) {
    if (me.src_kind != 1 || me.src_key < 0 || me.src_key >= 400 || me.vertices.empty() || me.indices.size() < 3) continue;
    StageMesh s;
    s.slot = me.src_key;
    s.part = me.src_record;
    s.strip_indices = me.strip_indices;
    s.index_groups = me.index_groups;
    s.indices = me.indices;
    // The game's UV: the diffuse UV is S1; a mesh whose S1 is ONE constant pair (0, 1) while S2 varies keeps the real diffuse UV in S2, with or without a light map
    // (the viewer's loader swaps only without one: a light-map mesh needs S2 as ITS set; the game's vertex has one UV and lights the map from the world position).
    // [C: tools\arena\arena_ours_test on the Check B capture, uv class report; the loader's own swap has already happened where it applies.]
    bool s1_const = me.vertices.size() > 1, s2_vary = false;
    for (size_t i = 1; i < me.vertices.size() && s1_const; ++i) {
      s1_const = me.vertices[i].uv0[0] == me.vertices[0].uv0[0] && me.vertices[i].uv0[1] == me.vertices[0].uv0[1];
      s2_vary = s2_vary || me.vertices[i].uv1[0] != me.vertices[0].uv1[0] || me.vertices[i].uv1[1] != me.vertices[0].uv1[1];
    }
    const bool uv_from_s2 = s1_const && s2_vary;
    if (me.src_skin_pairs == 1 && me.src_skin_slots.size() == 4 * me.vertices.size() && me.src_skin_weights.size() == 4 * me.vertices.size()) {  // S101 lane 49
      std::vector<uint8_t> ids, weights;
      bool all = true;
      for (size_t i = 0; i < me.vertices.size() && all; ++i) {
        prop_host::SkinWords words;
        all = prop_host::SinglePairWords(me.src_skin_pairs, &me.src_skin_slots[4 * i], &me.src_skin_weights[4 * i], prop_host::Faults{}, words);
        if (all) ids.insert(ids.end(), words.ids, words.ids + 4), weights.insert(weights.end(), words.weights, words.weights + 4);
      }
      if (all) s.skinned = true, s.skin_ids = std::move(ids), s.skin_weights = std::move(weights);
    }
    s.vertices.resize(me.vertices.size());
    for (size_t i = 0; i < me.vertices.size(); ++i) {
      const assets::Vertex& v = me.vertices[i];
      Vertex& d = s.vertices[i];
      const float ys = viewer_space ? 1.f : -1.f;  // the viewer's Y mirror (FinishModel) taken back off: the game's space
      d.pos[0] = v.pos[0], d.pos[1] = ys * v.pos[1], d.pos[2] = v.pos[2];
      d.normal[0] = v.normal[0], d.normal[1] = ys * v.normal[1], d.normal[2] = v.normal[2];
      std::memcpy(d.color, &v.color, 4);
      d.uv[0] = uv_from_s2 ? v.uv1[0] : v.uv0[0], d.uv[1] = uv_from_s2 ? v.uv1[1] : v.uv0[1];
      s.uv1.push_back(v.uv1[0]), s.uv1.push_back(v.uv1[1]);
    }
    if (me.material >= 0 && size_t(me.material) < model.materials.size()) {
      const Material& mt = model.materials[size_t(me.material)];
      s.effect = mt.shader;
      s.diffuse = map_tex(mt.diffuse), s.normal_map = map_tex(mt.normal), s.specular = map_tex(mt.specular);
      s.lm = mt.has_lm_slot;
      uint64_t h = 0xcbf29ce484222325ull;
      const auto mix = [&h](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
      };
      mix(mt.ambient, sizeof mt.ambient), mix(mt.diffuse_color, sizeof mt.diffuse_color), mix(mt.light_limit, sizeof mt.light_limit), mix(mt.spec_col, sizeof mt.spec_col);
      mix(&mt.spec_lev, 4), mix(&mt.spec_pow, 4), mix(mt.shader.data(), mt.shader.size());
      const uint8_t flags[4] = {uint8_t(mt.has_lm_slot), uint8_t(mt.alpha_test), uint8_t(mt.alpha_blend), uint8_t(mt.additive | (mt.cull_back << 1) | (mt.blend_inline << 2))};
      mix(flags, 4);
      s.material_hash = h;
      s.alpha_test = mt.alpha_test, s.alpha_blend = mt.alpha_blend, s.additive = mt.additive;
    }
    out.meshes.push_back(std::move(s));
  }
  out.stage_lightmap = map_tex(model.stage_lightmap);
  if (out.meshes.empty()) return err = "arena_game_space: arena " + std::to_string(arena) + " has no stage meshes", false;
  return true;
}

}  // namespace assets::arena_gs
