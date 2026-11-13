// See ring_game_space.h. Session 101, lane 70.
// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like arena_game_space.cpp.
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
#include <cmath>
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "ring_game_space.h"

#include <filesystem>
#include <map>

#include "asset_load.h"

namespace assets::ring_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;
}  // namespace

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(int arena, RingArena& out, std::string& err) {
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // one loader at a time (the arena's and the crowd's workers share the pac cache)
  if (!g_ready) return err = "ring_game_space: no game files (Init first)", false;
  out = RingArena{};
  out.arena = arena;
  const pac::Pac* p = load_detail::OpenCached(g_files, load_detail::ArenaRel(arena), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes& buf = p->bytes;
  TexSet tex;
  for (uint32_t k : {400u, 401u, 402u})
    if (const pac::Ent* e = pac::Find(h, k)) tex.AddDir(pac::Decode(buf.data() + e->off, e->size));
  Model model;   // only the materials / textures of the ring meshes are used from here; the geometry comes from the raw records
  std::map<int, int> remap;
  auto map_tex = [&](int idx) -> int {
    if (idx < 0 || size_t(idx) >= model.textures.size()) return -1;
    auto it = remap.find(idx);
    if (it == remap.end()) {
      out.textures.push_back(model.textures[size_t(idx)]);
      it = remap.emplace(idx, int(out.textures.size()) - 1).first;
    }
    return it->second;
  };
  for (int key : kEntries) {
    const pac::Ent* e = pac::Find(h, uint32_t(key));
    if (!e) continue;
    auto blob = std::make_shared<Bytes>(pac::Decode(buf.data() + e->off, e->size));
    jboy_parts::Parts parts;
    std::string perr;
    if (!jboy_parts::Parse(*blob, parts, perr)) continue;
    // The loader gives each mesh record its material; a record it skipped leaves no material, so the records are matched by order among the ones it kept.
    JboyOptions o;
    o.arena = true;
    o.part = "ring " + std::to_string(key);
    o.kind = 1, o.key = key, o.pac = load_detail::BaseName(load_detail::ArenaRel(arena));
    o.tex = &tex;
    const size_t before = model.meshes.size();
    std::string e2;
    if (!LoadJboy(*blob, o, model, e2)) continue;
    for (size_t mi = before; mi < model.meshes.size(); ++mi) {
      const Mesh& me = model.meshes[mi];
      if (me.src_record < 0 || size_t(me.src_record) >= parts.parts.size()) continue;
      const jboy_parts::Part& pt = parts.parts[size_t(me.src_record)];
      if (!pt.ok || !pt.tiles) continue;
      RingMesh r;
      r.entry = key, r.part = me.src_record, r.effect = pt.effect, r.blob = blob, r.data = pt, r.vertices = pt.vertices, r.pairs = pt.stride[1] / 8;
      if (me.material >= 0 && size_t(me.material) < model.materials.size()) {
        const Material& mt = model.materials[size_t(me.material)];
        r.diffuse = map_tex(mt.diffuse), r.normal_map = map_tex(mt.normal), r.specular = map_tex(mt.specular);
      }
      for (uint32_t v = 0; v < pt.vertices && (r.first_neg_u < 0 || r.first_strict_neg_u < 0); ++v) {   // S101 lane 220: the SIGN bit (a file u of -0.0 is stored signed in the stream, patch 109), and separately the first STRICTLY negative one
        const float fu = BeF(pt.s[2] + 8ull * v);
        if (r.first_neg_u < 0 && std::signbit(fu)) r.first_neg_u = int(v);
        if (r.first_strict_neg_u < 0 && fu < 0.0f) r.first_strict_neg_u = int(v);
      }
      out.meshes.push_back(std::move(r));
    }
  }
  if (out.meshes.empty()) return err = "ring_game_space: arena " + std::to_string(arena) + " has no ring entries", false;
  return true;
}

}  // namespace assets::ring_gs
