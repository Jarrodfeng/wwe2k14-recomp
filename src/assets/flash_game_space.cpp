// See flash_game_space.h. Session 101, lane 68.
#include "flash_game_space.h"

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like arena_game_space.cpp / crowd_body.cpp (asset_load.h pulls it in).
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

#include <cstring>
#include <filesystem>

#include "asset_load.h"

namespace assets::flash_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;
}  // namespace

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(int arena, FlashMesh& out, std::string& err) {
  using namespace load_detail;
  std::lock_guard<std::recursive_mutex> load_lock(LoadMutex());
  if (!g_ready) return err = "flash_game_space: no game files (Init first)", false;
  out = FlashMesh{};
  out.arena = arena;
  const pac::Pac* p = OpenCached(g_files, ArenaRel(arena), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes& buf = p->bytes;
  TexSet tex;
  for (uint32_t k : {400u, 401u, 402u})
    if (const pac::Ent* e = pac::Find(h, k)) tex.AddDir(pac::Decode(buf.data() + e->off, e->size));
  const pac::Ent* e = pac::Find(h, 31000u);
  if (!e) return err = "arena " + std::to_string(arena) + ": no entry 31000 (the crowd flash)", false;
  const Bytes blob = pac::Decode(buf.data() + e->off, e->size);
  if (!IsJboy(blob)) return err = "arena " + std::to_string(arena) + ": entry 31000 is not a JBOY", false;
  Model model;
  JboyOptions o;
  o.arena = true;
  o.part = "crowd flash 31000";
  o.kind = 1, o.key = 31000, o.pac = BaseName(ArenaRel(arena));
  o.tex = &tex;
  std::string e2;
  if (!LoadJboy(blob, o, model, e2)) return err = "arena " + std::to_string(arena) + ": crowd flash 31000 failed: " + e2, false;
  for (const Mesh& me : model.meshes) {
    if (me.src_kind != 1 || me.src_key != 31000 || me.vertices.empty() || me.indices.size() < 3) continue;
    out.strip_indices = me.strip_indices;
    out.index_groups = me.index_groups;
    out.indices = me.indices;
    out.vertices.resize(me.vertices.size());
    for (size_t i = 0; i < me.vertices.size(); ++i) {
      const assets::Vertex& v = me.vertices[i];
      arena_gs::Vertex& d = out.vertices[i];
      d.pos[0] = v.pos[0], d.pos[1] = v.pos[1], d.pos[2] = v.pos[2];   // (FinishModel, which applies the viewer Y mirror, is not run here: the loader vertices are the file space = the game space)
      d.normal[0] = v.normal[0], d.normal[1] = v.normal[1], d.normal[2] = v.normal[2];
      std::memcpy(d.color, &v.color, 4);
      d.uv[0] = v.uv0[0], d.uv[1] = v.uv0[1];
    }
    if (me.material >= 0 && size_t(me.material) < model.materials.size()) {
      const Material& mt = model.materials[size_t(me.material)];
      out.effect = mt.shader;
      if (mt.diffuse >= 0 && size_t(mt.diffuse) < model.textures.size()) out.has_diffuse = true, out.diffuse = model.textures[size_t(mt.diffuse)];
    }
    return true;   // mesh 0 (the model holds one)
  }
  return err = "arena " + std::to_string(arena) + ": entry 31000 holds no mesh", false;
}

}  // namespace assets::flash_gs
