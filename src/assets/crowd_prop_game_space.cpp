// See crowd_prop_game_space.h. Session 101, lane 133.
#include "crowd_prop_game_space.h"

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like flash_game_space.cpp / crowd_body.cpp (asset_load.h pulls pac.h in).
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
#include "prop_host_vertex.h"

namespace assets::crowd_prop_gs {
namespace {
launcher::GameFiles g_files;
bool g_ready = false;

constexpr uint32_t kFirstKey = 40000, kEndKey = 43000;
constexpr uint32_t kSetEntry = 20000;   // the audience set inside PACH 0
}  // namespace

bool ConvertJboy(const uint8_t* data, size_t size, PropMesh& out, std::string& err) {
  out = PropMesh{};
  if (size < 0x60 || std::memcmp(data, "JBOY", 4) != 0) return err = "not a JBOY entry", false;
  const uint8_t* b = data + 8;
  const size_t n_body = size - 8;
  const auto u32 = [&](size_t off) -> uint32_t { return off + 4 <= n_body ? Be32(b + off) : 0u; };
  if (u32(0x10) != 1) return err = "not a single-mesh model", false;
  const size_t rec = u32(0x14);
  if (rec + 0xB8 > n_body) return err = "mesh table past the end", false;
  const uint32_t n = u32(rec);
  const size_t s0 = u32(rec + 0x68), s1 = u32(rec + 0x6C), s2 = u32(rec + 0x70), ix = u32(rec + 0x9C);
  if (n == 0 || n > 65535) return err = "vertex count", false;
  if (s0 + 4 + size_t(n) * 28 > n_body || s1 != s0 + 4 + size_t(n) * 28) return err = "stream 0 is not 28 bytes a vertex", false;
  if (s2 == 0 || s2 + size_t(n) * 8 > n_body) return err = "no uv stream", false;
  if (ix == 0 || ix + 12 > n_body) return err = "no index block", false;
  const uint32_t prim = uint32_t(b[ix]) | uint32_t(b[ix + 1]) << 8 | uint32_t(b[ix + 2]) << 16 | uint32_t(b[ix + 3]) << 24;
  const uint32_t count = u32(ix + 4);
  if (prim != 6 || count < 3 || ix + 12 + size_t(count) * 2 > n_body) return err = "index block", false;
  out.vertex_count = n;
  out.vertices.resize(size_t(n) * kVertexBytes);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* v = b + s0 + 4 + size_t(i) * 28;
    float w[6];
    for (int c = 0; c < 6; ++c) w[c] = BeF(v + 4 * c);
    const uint8_t* t = b + s2 + size_t(i) * 8;
    const uint16_t h[2] = {prop_host::FloatToHalfTruncate(BeF(t)), prop_host::FloatToHalfTruncate(BeF(t + 4))};
    const float p[3] = {w[0] * 10.0f, w[1] * 10.0f, w[2] * 10.0f};   // float32 multiply: bit-equal to the stream's position
    uint8_t* d = out.vertices.data() + size_t(i) * kVertexBytes;
    std::memcpy(d, p, 12);
    std::memcpy(d + 12, w + 3, 12);
    std::memcpy(d + 24, h, 4);
  }
  out.indices.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    out.indices[i] = Be16(b + ix + 12 + size_t(i) * 2);
    if (out.indices[i] >= n) return err = "an index past the vertices", false;
  }
  return true;
}

bool Init(const std::string& game_path) {
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool Load(int arena, PropSet& out, std::string& err) {
  using namespace load_detail;
  std::lock_guard<std::recursive_mutex> load_lock(LoadMutex());
  if (!g_ready) return err = "crowd_prop_game_space: no game files (Init first)", false;
  out = PropSet{};
  out.arena = arena;
  const pac::Pac* p = OpenCached(g_files, ArenaRel(arena), err);
  if (!p) return false;
  Bytes sb;
  pac::Pach sp;
  if (!pac::GetNested(p->bytes, p->pachs[0], kSetEntry, sb, sp)) return err = "arena " + std::to_string(arena) + ": no audience set (entry 20000)", false;
  for (const pac::Ent& e : sp.ents) {
    if (e.id < kFirstKey || e.id >= kEndKey) continue;
    const Bytes blob = pac::Decode(sb.data() + e.off, e.size);
    if (!IsJboy(blob)) continue;
    PropMesh m;
    std::string why;
    if (!ConvertJboy(blob.data(), blob.size(), m, why)) continue;   // (multi-mesh or other models in the range stay out)
    m.key = e.id;
    out.meshes.push_back(std::move(m));
  }
  std::sort(out.meshes.begin(), out.meshes.end(), [](const PropMesh& a, const PropMesh& b) { return a.key < b.key; });
  if (out.meshes.empty()) return err = "arena " + std::to_string(arena) + ": no prop meshes in the audience set", false;
  return true;
}

}  // namespace assets::crowd_prop_gs
