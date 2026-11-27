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
#include "dds.h"
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
    constexpr float s = create_scale::kCrowdCreateScale;   // S111 MATLIVE review: the crowd's model-create scale (model_create_scale.h), 10
    const float p[3] = {w[0] * s, w[1] * s, w[2] * s};   // float32 multiply: bit-equal to the stream's position
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

bool ConvertDds(const uint8_t* data, size_t size, PropPicture& out, std::string& err) {
  out = PropPicture{};
  const size_t scan = size < 4096 ? size : 4096;
  size_t at = scan;
  for (size_t i = 0; i + 4 <= scan; ++i)
    if (std::memcmp(data + i, "DDS ", 4) == 0) { at = i; break; }
  if (at >= scan) return err = "no DDS in the entry", false;
  assets::Texture t;
  std::string why;
  if (!assets::ParseDds(data + at, size - at, t, why) && t.data.empty()) return err = why, false;
  const size_t mip0 = assets::MipBytes(t.format, t.width, t.height);
  if (t.format != assets::TexFormat::kBC1 && t.format != assets::TexFormat::kBC2 && t.format != assets::TexFormat::kBC3) return err = "not a DXT picture", false;
  if (!mip0 || t.data.size() < mip0) return err = "mip 0 is short", false;
  out.width = t.width, out.height = t.height;
  out.format = t.format == assets::TexFormat::kBC1 ? 18u : t.format == assets::TexFormat::kBC2 ? 19u : 20u;
  out.bytes.assign(t.data.begin(), t.data.begin() + std::ptrdiff_t(mip0));
  return true;
}

bool Init(const std::string& game_path) {
  std::lock_guard<std::recursive_mutex> load_lock(load_detail::LoadMutex());   // S103 D4: two threads call this now (the live crowd's worker and the old hook)
  static std::string g_init_root;
  if (g_ready && g_init_root == game_path) return true;   // same folder: nothing to redo (as crowd_body::Init)
  g_init_root = game_path;
  g_files.SetRoot(std::filesystem::u8path(game_path));
  g_ready = g_files.Exists("pac\\ch") || g_files.Exists("default.xex");
  return g_ready;
}

bool LoadSignPictures(std::vector<PropPicture>& out, std::string& err) {
  using namespace load_detail;
  std::lock_guard<std::recursive_mutex> load_lock(LoadMutex());
  static std::vector<PropPicture> s_signs;   // once per boot
  static std::string s_err;
  static bool s_done = false;
  if (!s_done) {
    if (!g_ready) return err = "crowd_prop_game_space: no game files (Init first)", false;   // (not cached: a later call may have the root)
    s_done = true;
    pac::Pac p;
    std::string e;
    if (!pac::Open(g_files, "pac\\audience.pac", p, e)) s_err = e;
    else {
      Bytes nb;
      pac::Pach np;
      if (p.pachs.empty() || !pac::GetNested(p.bytes, p.pachs[0], 0, nb, np)) s_err = "pac\\audience.pac: PACH 0 has no entry 0 (the sign pictures)";
      else {
        for (const pac::Ent& en : np.ents) {
          if (en.id == 0 || en.id >= 65000) continue;   // (a key of 65000 or more is the game's runtime texture, never a file picture)
          const Bytes blob = pac::Decode(nb.data() + en.off, en.size);
          PropPicture pp;
          std::string pwhy;
          if (ConvertDds(blob.data(), blob.size(), pp, pwhy) && pp.format == 18 && pp.width == 64 && pp.height == 64) pp.key = en.id, s_signs.push_back(std::move(pp));   // (every file sign is 64x64 DXT1; anything else stays a guest read)
        }
        std::sort(s_signs.begin(), s_signs.end(), [](const PropPicture& a, const PropPicture& b) { return a.key < b.key; });
        if (s_signs.empty()) s_err = "pac\\audience.pac: no usable sign picture";
      }
    }
  }
  if (s_signs.empty()) return err = s_err.empty() ? "no sign pictures" : s_err, false;
  out = s_signs;
  return true;
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
    if (!IsJboy(blob)) {   // S103 FHB: a texture entry (the foam hands' prints 42000 + h - 1) is kept as a picture
      PropPicture pp;
      std::string pwhy;
      if (ConvertDds(blob.data(), blob.size(), pp, pwhy)) pp.key = e.id, out.pictures.push_back(std::move(pp));
      continue;
    }
    PropMesh m;
    std::string why;
    if (!ConvertJboy(blob.data(), blob.size(), m, why)) continue;   // (multi-mesh or other models in the range stay out)
    m.key = e.id;
    out.meshes.push_back(std::move(m));
  }
  std::sort(out.meshes.begin(), out.meshes.end(), [](const PropMesh& a, const PropMesh& b) { return a.key < b.key; });
  std::sort(out.pictures.begin(), out.pictures.end(), [](const PropPicture& a, const PropPicture& b) { return a.key < b.key; });
  if (out.meshes.empty()) return err = "arena " + std::to_string(arena) + ": no prop meshes in the audience set", false;
  std::string sgn;
  if (!LoadSignPictures(out.sign_pictures, sgn)) out.sign_pictures.clear();   // (a missing audience.pac is not an arena failure: the signs keep the guest read)
  return true;
}

}  // namespace assets::crowd_prop_gs
