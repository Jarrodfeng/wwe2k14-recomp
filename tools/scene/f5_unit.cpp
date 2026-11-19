// S103 lane F5: offline unit of the particle namer's game-side parts, on the player's OWN prtcls.pac (read in place, never copied or committed; the output is counts and names only).
//   f5_unit.exe GAME_ROOT [--plant tiled|endian|index_first]
// 1. the sprite library: every PCE6 sprite directory is enumerated (assets/particle_sprites.h ForEachDirectory) and a NameIndex built from them (particle_namer.h);
// 2. LoadSprite(entry) round trip: a named entry loads back, and its DDS key is the index's key;
// 3. the guest side: EVERY indexed sprite texture is forged into guest memory the way the 360 stores it (tiled, endian swapped, fetch dwords, registry + wrapper + header) and the GuestContentHasher /
//    HandleTextureNamer must name it back to a file with the SAME bytes (the untile / swap / hash pipeline vs the DDS key, on all of the player's sprites); the planted twins (tiled bit ignored,
//    endian swap skipped) must stop naming them;
// 4. what the namer cannot name is COUNTED (small mip-tail textures, formats), never guessed.
// This proves the pipeline against files; whether the game's memory has exactly this layout is the adapter's arithmetic (guest_texture.h copies it) and the live census (namer line) [U until a run].
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/particle_sprites.h"
#include "../../src/scene_live/particle_hasher.h"
#include "../../src/scene_live/particle_namer.h"

using namespace rdr_scene::live;
using namespace rdr_scene::live::particles;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what, const std::string& detail = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  : ", detail.c_str());
  ok ? ++g_pass : ++g_fail;
}

struct Mem final : GuestMem {
  std::map<uint32_t, uint32_t> w;
  bool Read32(uint32_t a, uint32_t& out) const override {
    const auto it = w.find(a);
    if (it == w.end()) return false;
    out = it->second;
    return true;
  }
};

// the pixel memory at a physical base: a sparse vector
struct Pixels {
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
  bool Read(uint32_t a, uint32_t n, uint8_t* d) const {
    if (a < base || uint64_t(a - base) + n > bytes.size()) return false;
    std::memcpy(d, bytes.data() + (a - base), n);
    return true;
  }
};

// Forge one texture into guest memory: tiled + endian 2 (the 8-in-32 swap the sprites use), pitch aligned as GuestLevelLayout does. Returns false when the texture cannot be forged.
static bool Forge(const assets::Texture& t, Mem& m, Pixels& px, uint32_t id, uint32_t wrapper, uint32_t header, uint32_t base, bool packed_bit) {
  const uint32_t bpb = t.format == assets::TexFormat::kBC1 ? 8 : 16;
  const uint32_t fmt = t.format == assets::TexFormat::kBC1 ? 18 : t.format == assets::TexFormat::kBC2 ? 19 : 20;
  const uint32_t bx = (t.width + 3) / 4, by = (t.height + 3) / 4;
  const uint32_t pitch_div32 = (t.width + 31) / 32;   // the fetch constant counts the pitch in 32-texel units
  const uint32_t pitch_blocks = (((pitch_div32 << 5) + 3) / 4 + 31) & ~31u;
  if (pitch_blocks < bx) return false;
  const uint32_t log2 = bpb == 8 ? 3u : 4u;
  uint32_t top = 0;
  for (uint32_t y = 0; y < by; ++y)
    for (uint32_t x = 0; x < bx; ++x) top = std::max(top, guest_tex::TiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2) + bpb);
  px.base = base;
  px.bytes.assign(top + 4096, 0);
  for (uint32_t y = 0; y < by; ++y)
    for (uint32_t x = 0; x < bx; ++x) {
      uint8_t blk[16];
      std::memcpy(blk, t.data.data() + (size_t(y) * bx + x) * bpb, bpb);
      for (uint32_t i = 0; i + 4 <= bpb; i += 4) std::swap(blk[i], blk[i + 3]), std::swap(blk[i + 1], blk[i + 2]);   // endian 2 (8-in-32): the forge's own swap (the plants switch guest_tex's off)
      std::memcpy(px.bytes.data() + guest_tex::TiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2), blk, bpb);
    }
  const uint32_t reg = 0x2000, entries = 0x3000;
  m.w.clear();
  m.w[kTextureHandleGlobal] = reg;
  m.w[reg + 8] = entries, m.w[reg + 16] = entries + 8 * 0x10000u;
  const uint32_t idx = id & 0xFFFF;
  m.w[entries + idx * 8] = id, m.w[entries + idx * 8 + 4] = wrapper;
  m.w[wrapper] = kWrapperVtableA, m.w[wrapper + kWrapperKind] = 0, m.w[wrapper + kWrapperTexture] = header;
  const uint32_t f[6] = {0x80000000u | (pitch_div32 << 22), fmt | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(base) & 0xFFFFF000u), (t.width - 1) | ((t.height - 1) << 13), 0, 0, (1u << 9) | (packed_bit ? (1u << 11) : 0)};
  for (int i = 0; i < 6; ++i) m.w[header + kHeaderFetch + 4 * i] = f[i];
  return true;
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : std::string();
  std::string plant, menu_dir;
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--plant") plant = argv[i + 1];
    if (std::string(argv[i]) == "--menu") menu_dir = argv[i + 1];
  }
  assets::particle_sprites::g_pce6_only = plant == "pce6only";
  assets::particle_sprites::g_skip_pcge = plant == "nopcge";
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(root));
  assets::pac::Pac p;
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  if (!assets::pac::Open(files, "pac\\prtcls.pac", p, err)) { std::printf("FAIL  open prtcls.pac: %s\n", err.c_str()); return 2; }
  const auto t1 = std::chrono::steady_clock::now();
  NameIndex idx;
  std::vector<std::string> leaves;
  uint32_t dirs = assets::particle_sprites::ForEachDirectory(p, [&](const std::string& leaf, const uint8_t* d, size_t n) {
    idx.AddSpriteDirectory(d, n, "pac\\prtcls.pac", leaf);
    leaves.push_back(leaf);
  });
  const auto t2 = std::chrono::steady_clock::now();
  const auto ms = [](auto a, auto b) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
  std::printf("info: prtcls.pac open %lld ms, %u sprite directories walked + indexed in %lld ms (the live boot builds this on a worker thread), %zu distinct textures, %zu duplicates\n", ms(t0, t1), dirs, ms(t1, t2), idx.size(), idx.duplicates());
  Check(dirs > 100 && idx.size() > 5, "the player's prtcls.pac has sprite directories and the index holds distinct textures", std::to_string(dirs) + " dirs, " + std::to_string(idx.size()) + " textures");

  // 1b. CAWH: the walk covers PCE6, PCMN and PCCH (the menu / editor particles live in PCMN; a PCE6-only walk named 0 of 52346 handles in the CAW editor)
  {
    size_t np = 0, nm = 0, nc = 0;
    bool p0107 = false, smoke4 = false, smoke4_loads = false;
    for (const std::string& leaf : leaves) {
      np += leaf[0] == 'p', nm += leaf[0] == 'm', nc += leaf[0] == 'c';
      p0107 = p0107 || leaf.compare(0, 6, "p0107/") == 0;
    }
    assets::particle_sprites::ForEachDirectory(p, [&](const std::string& leaf, const uint8_t* d, size_t n) {
      if (leaf[0] != 'm') return;
      const uint32_t count = *reinterpret_cast<const uint32_t*>(d);
      for (uint32_t i = 0; i < count && 0x10 + size_t(i + 1) * 32 <= n; ++i) {
        const uint8_t* r = d + 0x10 + size_t(i) * 32;
        if (std::string(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16)) != "Smoke0004") continue;
        smoke4 = true;
        assets::Texture t;
        std::string why;
        smoke4_loads = smoke4_loads || assets::particle_sprites::LoadSprite(p, leaf + "/Smoke0004", t, why);
      }
    });
    Check(idx.size() >= 82 + 18 + 13, "CAWH: the index holds the PCE6 (82), PCMN (18) and PCCH (13 more) distinct pictures", std::to_string(idx.size()) + " keys; leaves p " + std::to_string(np) + ", m " + std::to_string(nm) + ", c " + std::to_string(nc));
    Check(smoke4 && smoke4_loads, "CAWH: PCMN Smoke0004 has an m-prefixed leaf and LoadSprite decodes it");
    Check(p0107, "CAWH: PCE6 names are unchanged (p0107/...)");
    {   // no PCE6 regression: every picture a PCE6 directory holds is still named by a PCE6 ('p') leaf (the first leaf with the bytes wins, PCE6 is walked first)
      size_t pk = 0, pk_p = 0;
      assets::particle_sprites::ForEachDirectory(p, [&](const std::string& leaf, const uint8_t* d, size_t n) {
        if (leaf[0] != 'p') return;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(d);
        for (uint32_t i = 0; i < count && 0x10 + size_t(i + 1) * 32 <= n; ++i) {
          const uint8_t* r = d + 0x10 + size_t(i) * 32;
          const uint32_t size = *reinterpret_cast<const uint32_t*>(r + 20), off = *reinterpret_cast<const uint32_t*>(r + 24);
          uint64_t key = 0;
          if (off > n || size > n - off || !DdsMipZeroKey(d + off, size, key)) continue;
          const NameIndex::Name* nm = idx.Find(key);
          ++pk, pk_p += nm && !nm->entry.empty() && nm->entry[0] == 'p';
        }
      });
      Check(pk > 0 && pk == pk_p, "CAWH: every PCE6 picture is still named by a PCE6 leaf (no regression of the match smoke / entrance names)", std::to_string(pk_p) + " of " + std::to_string(pk));
    }
    if (!menu_dir.empty()) {   // past-bug re-find: the c2d_menu capture's particle images (tools\particles\menu_particle_dump.py): 0 of 10 named before the fix
      size_t files_n = 0, hit = 0;
      for (const auto& e : std::filesystem::directory_iterator(menu_dir)) {
        std::FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (!f) continue;
        uint32_t hd[3] = {};
        std::vector<uint8_t> bytes(size_t(std::filesystem::file_size(e.path())));
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size() || bytes.size() < 12) continue;
        std::memcpy(hd, bytes.data(), 12);
        ++files_n;
        const size_t blocks = size_t((hd[0] + 3) / 4) * ((hd[1] + 3) / 4), len = bytes.size() - 12;
        const uint32_t fcs[2] = {len == blocks * 8 ? hd[2] : 0x33545844u, hd[2]};   // 16-byte blocks: DXT3 or DXT5
        bool found = false;
        for (uint32_t fc : fcs) found = found || idx.Find(ContentKey(hd[0], hd[1], fc, bytes.data() + 12, len)) != nullptr;
        hit += found;
      }
      Check(files_n == 10 && hit == files_n, "CAWH past-bug re-find: the c2d_menu capture's particle images are named by the index (0 of 10 with a PCE6-only walk)", std::to_string(hit) + " of " + std::to_string(files_n));
    }
  }

  // 1c. INN: the PCGE named banks (INFE the Inferno ring fire, GAME, RYAL, ECW_) are walked ('g' leaves, the 4 bank chars verbatim); a walk that skips PCGE (the old one, --plant nopcge) misses them
  {
    size_t by_bank[4] = {};
    const char* banks[4] = {"gGAME/", "gRYAL/", "gECW_/", "gINFE/"};
    for (const std::string& leaf : leaves)
      for (int b = 0; b < 4; ++b) by_bank[b] += leaf.compare(0, 6, banks[b]) == 0;
    bool all_banks = true;
    std::string counts;
    for (int b = 0; b < 4; ++b) all_banks = all_banks && by_bank[b] > 0, counts += std::string(banks[b]) + std::to_string(by_bank[b]) + " ";
    Check(all_banks, "INN: every non-empty PCGE bank (GAME, RYAL, ECW_, INFE) has sprite directories in the walk", counts);
    assets::Texture t;
    std::string why;
    const std::string glow = "gINFE/110/1/Light0001";   // the big-fire glow: no byte-identical twin in PCE6 / PCMN / PCCH (INF.md)
    const bool loads = assets::particle_sprites::LoadSprite(p, glow, t, why);
    bool in_index = false;
    if (loads) {
      const uint32_t fourcc = t.format == assets::TexFormat::kBC1 ? 0x31545844u : t.format == assets::TexFormat::kBC2 ? 0x33545844u : 0x35545844u;
      const size_t bpb = t.format == assets::TexFormat::kBC1 ? 8 : 16;
      const NameIndex::Name* nm = idx.Find(ContentKey(t.width, t.height, fourcc, t.data.data(), size_t((t.width + 3) / 4) * ((t.height + 3) / 4) * bpb));
      in_index = nm && nm->entry == glow;
    }
    Check(loads && in_index, "INN: gINFE/110/1/Light0001 loads back and is in the index under that leaf", loads ? std::string("loaded") : why);
  }

  // 2. round trip: every distinct key's entry loads and hashes to its own key
  size_t rt_ok = 0, rt_bad = 0, light0001 = 0;
  std::vector<std::pair<std::string, assets::Texture>> all;   // one texture per distinct key
  {
    std::map<uint64_t, bool> seen;
    for (const std::string& leaf : leaves) {
      // list the names of the directory through LoadSprite's own parser is not exposed: walk again
    }
    assets::particle_sprites::ForEachDirectory(p, [&](const std::string& leaf, const uint8_t* d, size_t n) {
      const uint32_t count = *reinterpret_cast<const uint32_t*>(d);
      for (uint32_t i = 0; i < count && 0x10 + size_t(i + 1) * 32 <= n; ++i) {
        const uint8_t* r = d + 0x10 + size_t(i) * 32;
        std::string nm(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
        uint32_t size = *reinterpret_cast<const uint32_t*>(r + 20), off = *reinterpret_cast<const uint32_t*>(r + 24);
        uint64_t key = 0;
        if (off > n || size > n - off || !DdsMipZeroKey(d + off, size, key)) continue;
        if (seen.emplace(key, true).second) {
          assets::Texture t;
          std::string why;
          const std::string entry = leaf + "/" + nm;
          if (assets::particle_sprites::LoadSprite(p, entry, t, why)) all.push_back({entry, std::move(t)});
          else ++rt_bad, std::printf("info: LoadSprite failed: %s (%s)\n", entry.c_str(), why.c_str());
        }
        if (nm == "Light0001") ++light0001;
      }
    });
    for (const auto& e : all) {
      const NameIndex::Name* n = nullptr;
      uint64_t key = 0;
      std::string pac, ent;
      (void)pac, (void)ent;
      // the entry loads; its content is in the index under SOME leaf with the same bytes
      const assets::Texture& t = e.second;
      const uint32_t fourcc = t.format == assets::TexFormat::kBC1 ? 0x31545844u : t.format == assets::TexFormat::kBC2 ? 0x33545844u : 0x35545844u;
      const size_t bpb = t.format == assets::TexFormat::kBC1 ? 8 : 16;
      key = ContentKey(t.width, t.height, fourcc, t.data.data(), size_t((t.width + 3) / 4) * ((t.height + 3) / 4) * bpb);
      n = idx.Find(key);
      n ? ++rt_ok : ++rt_bad;
    }
  }
  Check(rt_ok == all.size() && rt_bad == 0 && !all.empty(), "LoadSprite(entry) round trip: every distinct sprite loads by its entry name and its DDS key is in the index", std::to_string(rt_ok) + " of " + std::to_string(all.size()) + ", bad " + std::to_string(rt_bad));
  Check(light0001 > 0, "the player's pac holds Light0001 sprites (the texture of the run-22 handle)", std::to_string(light0001) + " directories");

  // 3. the guest side: forge every distinct texture and name it back
  guest_tex::g_tex_plant = plant == "tiled" ? 2 : plant == "endian" ? 3 : 0;
  Mem m;
  Pixels px;
  size_t forged = 0, named = 0, not_named = 0, refused_forge = 0, small = 0, mismatch = 0;
  const uint32_t id = 0x091903BD, wrapper = 0x1000, header = 0x1800, base = 0x10000000;
  for (const auto& e : all) {
    const assets::Texture& t = e.second;
    if (t.format != assets::TexFormat::kBC1 && t.format != assets::TexFormat::kBC2 && t.format != assets::TexFormat::kBC3) { ++refused_forge; continue; }
    const bool packed = t.mips > 1;
    if (!Forge(t, m, px, id, wrapper, header, base, packed)) { ++refused_forge; continue; }
    ++forged;
    if (packed && std::min(t.width, t.height) <= 16) ++small;
    GuestContentHasher<std::function<bool(uint32_t, uint32_t, uint8_t*)>> hs([&](uint32_t a, uint32_t n, uint8_t* d) { return px.Read(a, n, d); });
    HandleTextureNamer namer(m, hs, idx);
    std::string pac, ent;
    if (namer.Name(id, 0, pac, ent)) {
      ++named;
      // the named leaf must hold the SAME bytes (the first leaf with these bytes wins, so compare keys through LoadSprite)
      assets::Texture back;
      std::string why;
      const bool same = assets::particle_sprites::LoadSprite(p, ent, back, why) && back.width == t.width && back.height == t.height && back.data == t.data;
      if (!same) ++mismatch;
    } else {
      ++not_named;
    }
  }
  std::printf("info: forged %zu textures (refused to forge %zu), named %zu, not named %zu (small packed-mip textures %zu), named-to-different-bytes %zu\n", forged, refused_forge, named, not_named, small, mismatch);
  // the same check with or without a plant: a planted twin (tiled bit ignored / endian swap skipped) must FAIL it (exit 1)
  Check(forged > 0 && mismatch == 0 && named > 0 && not_named <= small,
        "the guest pipeline names every forged sprite texture back to a file with the same bytes (the only ones it may leave unnamed are small packed-mip-tail textures, which are counted, not guessed)",
        std::to_string(named) + " named of " + std::to_string(forged) + ", unnamed " + std::to_string(not_named) + ", small " + std::to_string(small));
  guest_tex::g_tex_plant = 0;
  std::printf("f5_unit: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
