// See screen_game_space.h. S101 lane 115.
// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like ring_game_space.cpp (asset_load.h's inlines name it).
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

#include "screen_game_space.h"

#include <filesystem>

#include "asset_load.h"

namespace assets::screen_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;
}  // namespace

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(int arena, ScreenSet& out, std::string& err) {
  std::lock_guard<std::recursive_mutex> load_lock(assets::load_detail::LoadMutex());  // one loader at a time (the arena's and the crowd's workers share the pac cache)
  if (!g_ready) return err = "screen_game_space: no game files (Init first)", false;
  out = ScreenSet{};
  out.arena = arena;
  const pac::Pac* p = load_detail::OpenCached(g_files, load_detail::ArenaRel(arena), err);
  if (!p) return false;
  const pac::Pach& h = p->pachs[0];
  const Bytes& buf = p->bytes;
  for (int key = kFirstKey; key <= kLastKey; ++key) {
    const pac::Ent* e = pac::Find(h, uint32_t(key));
    if (!e) continue;
    auto blob = std::make_shared<Bytes>(pac::Decode(buf.data() + e->off, e->size));
    jboy_parts::Parts parts;
    std::string perr;
    if (!jboy_parts::Parse(*blob, parts, perr)) continue;
    bool any = false;
    for (const jboy_parts::Part& pt : parts.parts) {
      if (!pt.ok || !pt.tiles) continue;
      ring_gs::RingMesh r;   // the ring's mesh record: `entry` = the pac key; no material, no textures (every texture slot of a screen is the stream's)
      r.entry = key, r.part = int(pt.mesh_index), r.effect = pt.effect, r.blob = blob, r.data = pt, r.vertices = pt.vertices, r.pairs = pt.stride[1] / 8;
      for (uint32_t v = 0; v < pt.vertices && (r.first_neg_u < 0 || r.first_strict_neg_u < 0); ++v) {   // S101 lane 220: the SIGN bit (a file u of -0.0 is stored signed in the stream, patch 109), and separately the first STRICTLY negative one
        const float fu = BeF(pt.s[2] + 8ull * v);
        if (r.first_neg_u < 0 && std::signbit(fu)) r.first_neg_u = int(v);
        if (r.first_strict_neg_u < 0 && fu < 0.0f) r.first_strict_neg_u = int(v);
      }
      out.meshes.push_back(std::move(r));
      any = true;
    }
    if (any) out.keys.push_back(key);
  }
  if (out.meshes.empty()) return err = "screen_game_space: arena " + std::to_string(arena) + " has no screen entries (keys 800-809)", false;
  return true;
}

}  // namespace assets::screen_gs
