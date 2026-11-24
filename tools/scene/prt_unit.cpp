// S108 lane PRT (docs\research\s108\PRT_plan.md): a DLC entrance's particle pictures are drawn from the GAME's own texture when the file index (the disc's prtcls.pac) does not hold them.
// Offline, 0 game runs, on the player's own files read in place (nothing copied or written; the output is counts and names).
//   prt_unit.exe GAME_DATA [--plant index_only|no_packed_tail|mip_pitch]
// GAME_DATA = the folder holding extracted\pac\prtcls.pac and dlc\DLC Pack 3\pac\prtcls_dlc04.pac.
// The production path in the production order: the disc index (ForEachDirectory + AddSpriteDirectory, as BuildParticleSprites) -> a texture forged into guest memory the 360 way (registry entry,
// wrapper, header fetch dwords, tiled + endian-2 blocks, the mip chain at the mip address with a packed tail) -> HandleTextureNamer.Name (the real namer + GuestContentHasher) -> the batch's name ->
// ParticleSpriteFor + MakeSpriteImage (the render side's resolve, particle_sprite_lookup.h) -> LiveParticles::Record drawn by the scene front-end on a headless renderer.
// Checks: Fandango's beam_glow00 and the sign pyro's Cloud06 (prtcls_dlc04.pac, NOT in the disc index) are named from the guest, their every level byte-equal to the file's, and the batch draws;
// a disc Light0001 is named by the index and the dev self-check finds the guest chain equal; the store's bound holds; a guest name with no picture (offline replay) is reported as such.
// Plants (each must FAIL, exit 1): index_only (the old rule: an index miss stays unnamed, the DLC batch is skipped); no_packed_tail / mip_pitch (a wrong level layout: the chain differs from the file).
// The forge writes the levels with this file's OWN copy of the layout rules (the adapter's arithmetic, reader.h); that both sides follow one rule is the limit of an offline check: the live proof is the
// run's `guest_check equal / differ` on the disc sprites (particle_namer.h CheckAgainstFile).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/particle_sprites.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_particles.h"
#include "../../src/renderer_scene/particle_sprite_lookup.h"
#include "../../src/scene_live/particle_hasher.h"
#include "../../src/scene_live/particle_namer.h"

using namespace rdr_scene;
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
struct Pixels {
  uint32_t base = 0;
  std::vector<uint8_t> bytes;
  bool Read(uint32_t a, uint32_t n, uint8_t* d) const {
    if (a < base || uint64_t(a - base) + n > bytes.size()) return false;
    std::memcpy(d, bytes.data() + (a - base), n);
    return true;
  }
  uint8_t* At(uint32_t a, uint32_t n) {
    if (uint64_t(a - base) + n > bytes.size()) bytes.resize(size_t(a - base) + n, 0);
    return bytes.data() + (a - base);
  }
};

// ---- the forge: this file's own copy of the 360 layout rules ----------------------------------------------------------------------------------------------------------------------------------
static uint32_t L2c(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) ++r; return r; }
static uint32_t Al(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static void PutBlock(Pixels& px, uint32_t region, uint32_t off, const uint8_t* blk, uint32_t bpb) {
  uint8_t b[16];
  std::memcpy(b, blk, bpb);
  for (uint32_t i = 0; i + 4 <= bpb; i += 4) std::swap(b[i], b[i + 3]), std::swap(b[i + 1], b[i + 2]);   // endian 2 (8-in-32)
  std::memcpy(px.At(region + off, bpb), b, bpb);
}
// One texture with its whole chain: mip 0 tiled at `base`, levels 1.. at `mip_base` (non-base pitch from the next power of two, 32-block rows, 4 KB slices), levels >= the packed level in one tail.
static bool Forge(const assets::Texture& t, Mem& m, Pixels& px, uint32_t id, uint32_t base, uint32_t mip_base) {
  const uint32_t bpb = t.format == assets::TexFormat::kBC1 ? 8 : 16, l2 = bpb == 8 ? 3u : 4u;
  const uint32_t fmt = t.format == assets::TexFormat::kBC1 ? 18 : t.format == assets::TexFormat::kBC2 ? 19 : 20;
  const uint32_t pitch_div32 = (t.width + 31) / 32, pitch0 = Al(Al(pitch_div32 << 5, 4) / 4, 32);
  const uint32_t log2min = L2c(std::min(t.width, t.height));
  if (log2min <= 4) return false;   // mip 0 in the tail: not what the sprites are
  const uint32_t packed_level = log2min - 4;
  px.base = 0x10000000u;
  px.bytes.clear();
  size_t at = 0;
  uint32_t level_off[16] = {}, acc = 0;
  for (uint32_t lv = 1; lv < t.mips && lv <= packed_level; ++lv) {
    level_off[lv] = acc;
    const uint32_t rw = std::max((1u << L2c(t.width)) >> lv, 1u), rh = std::max((1u << L2c(t.height)) >> lv, 1u);
    acc += Al(Al(Al(rw, 4) / 4, 32) * bpb * Al(Al(rh, 4) / 4, 32), 4096);
  }
  for (uint32_t lv = 0; lv < t.mips; ++lv) {
    const uint32_t lw = std::max(t.width >> lv, 1u), lh = std::max(t.height >> lv, 1u), bx = (lw + 3) / 4, by = (lh + 3) / 4;
    uint32_t region = base, pitch = pitch0, ox = 0, oy = 0;
    if (lv) {
      const uint32_t stored = std::min(lv, packed_level);
      region = mip_base + level_off[stored];
      pitch = Al(Al(std::max((1u << L2c(t.width)) >> stored, 1u), 4) / 4, 32);
      if (lv >= packed_level) {   // the tail offsets (in texels, then blocks)
        const uint32_t lw2 = L2c(t.width), lh2 = L2c(t.height), pk = lv - packed_level;
        if (pk < 3) (lw2 > lh2 ? oy : ox) = 16u >> pk;
        else if (lw2 > lh2) ox = (1u << (lw2 - packed_level)) >> (pk - 2);
        else oy = (1u << (lh2 - packed_level)) >> (pk - 2);
        ox /= 4, oy /= 4;
      }
    }
    for (uint32_t y = 0; y < by; ++y)
      for (uint32_t x = 0; x < bx; ++x) PutBlock(px, region, guest_tex::TiledOffset2D(int32_t(ox + x), int32_t(oy + y), pitch, l2), t.data.data() + at + (size_t(y) * bx + x) * bpb, bpb);
    at += size_t(bx) * by * bpb;
  }
  const uint32_t reg = 0x2000, entries = 0x3000, wrapper = 0x1000, header = 0x1800;
  m.w.clear();
  m.w[kTextureHandleGlobal] = reg;
  m.w[reg + 8] = entries, m.w[reg + 16] = entries + 8 * 0x10000u;
  m.w[entries + (id & 0xFFFF) * 8] = id, m.w[entries + (id & 0xFFFF) * 8 + 4] = wrapper;
  m.w[wrapper] = kWrapperVtableA, m.w[wrapper + kWrapperKind] = 0, m.w[wrapper + kWrapperTexture] = header;
  const uint32_t f[6] = {0x80000000u | (pitch_div32 << 22), fmt | (2u << 6) | (wwe2k14::guest_view::ToEViewCpu(base) & 0xFFFFF000u), (t.width - 1) | ((t.height - 1) << 13), 0,
                         (t.mips - 1) << 6, (wwe2k14::guest_view::ToEViewCpu(mip_base) & 0xFFFFF000u) | (1u << 9) | (1u << 11)};
  for (int i = 0; i < 6; ++i) m.w[header + kHeaderFetch + 4 * i] = f[i];
  return true;
}

static bool OpenPac(const std::string& path, assets::pac::Pac& p) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string err;
  return !raw.empty() && assets::pac::OpenBytes(std::move(raw), path, p, err);
}
static uint64_t KeyOf(const assets::Texture& t) {
  const uint32_t fc = t.format == assets::TexFormat::kBC1 ? 0x31545844u : t.format == assets::TexFormat::kBC2 ? 0x33545844u : 0x35545844u;
  return ContentKey(t.width, t.height, fc, t.data.data(), size_t((t.width + 3) / 4) * ((t.height + 3) / 4) * (t.format == assets::TexFormat::kBC1 ? 8 : 16));
}

// Name one forged texture through the real namer. Returns the name and, through `pic`, the store's picture for a guest name.
struct Named { bool ok = false; std::string pac, entry; };
static Named NameForged(const assets::Texture& t, const NameIndex& idx, GuestSprites& store, NamerStats* st_out = nullptr,
                        std::function<std::shared_ptr<const assets::Texture>(const std::string&)> file_sprite = nullptr) {
  Mem m;
  Pixels px;
  const uint32_t id = 0x096103E0u;   // the FAN log's beam_glow00 handle
  Named n;
  if (!Forge(t, m, px, id, 0x10000000u, 0x10100000u)) return n;
  GuestContentHasher<std::function<bool(uint32_t, uint32_t, uint8_t*)>> hs([&](uint32_t a, uint32_t k, uint8_t* d) { return px.Read(a, k, d); });
  HandleTextureNamer namer(m, hs, idx);
  namer.SetGuestStore(&store);
  if (file_sprite) namer.SetFileSprite(file_sprite);
  n.ok = namer.Name(id, 0, n.pac, n.entry);
  if (st_out) *st_out = namer.stats();
  return n;
}

// The batch drawn through LiveParticles with the lookup the frame hooks use. Returns the stats; `centre` = the centre pixel.
static LiveParticlesStats DrawBatch(rdr::Renderer& r, FrontEnd& fe, const Named& nm, const std::shared_ptr<const std::unordered_map<std::string, assets::Texture>>& files, const GuestSprites& store, uint8_t centre[3]) {
  rdr::ImageId img = rdr::kNoImage;
  if (nm.ok) {
    bool guest = false;
    const auto t = ParticleSpriteFor(nm.pac, nm.entry, files, store, guest);
    if (t) img = MakeSpriteImage(r, *t);
  }
  LiveParticles lp(r, [&](uint32_t asset) { return asset == 0 ? img : rdr::kNoImage; });
  lp.SetSkipUnnamed(true);   // the live drawer's setting (frame_hooks.h particles_skip_unnamed)
  const uint32_t frame = 900;
  lp.SetFrame(frame);
  ParticleTables pt;
  ParticleBatch b;
  b.first = 0, b.count = 1, b.texture = 0, b.blend = kParticleAdditive, b.frame = frame;   // glow_00: blend code 1 = additive
  pt.batches = {b};
  Particle p{};
  p.local_world[0] = 0.9f, p.local_world[5] = 0.9f, p.local_world[11] = 0.5f;
  const float cs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) {
    p.v[i].pos[0] = cs[i][0], p.v[i].pos[1] = cs[i][1];
    p.v[i].color = 0xFFFFFFFFu;
    p.v[i].uv[0] = cs[i][0] > 0 ? 1.f : 0.f, p.v[i].uv[1] = cs[i][1] > 0 ? 1.f : 0.f;
  }
  pt.particles = {p};
  SceneFrame sf;
  sf.width = sf.height = 64;
  sf.viewer_space = false;
  sf.clear[0] = sf.clear[1] = sf.clear[2] = 0.f, sf.clear[3] = 1.f;
  sf.camera.view = IdentityMat4();
  sf.camera.proj = IdentityMat4();
  sf.particles = &pt;
  sf.stage_hooks.push_back(&lp);
  std::vector<uint8_t> rgba;
  std::string err;
  centre[0] = centre[1] = centre[2] = 0;
  if (fe.RenderToPixels(sf, rgba, err) && rgba.size() >= 64 * 64 * 4) {
    // the brightest pixel of the frame (a glow's bright part is not always at the exact centre)
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      if (int(rgba[i]) + rgba[i + 1] + rgba[i + 2] > int(centre[0]) + centre[1] + centre[2]) centre[0] = rgba[i], centre[1] = rgba[i + 1], centre[2] = rgba[i + 2];
  }
  if (img != rdr::kNoImage) r.DestroyImage(img);
  return lp.stats();
}

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : std::string();
  std::string plant;
  for (int i = 2; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = argv[i + 1];
  g_namer_plant = plant == "index_only" ? 3 : 0;
  guest_tex::g_tex_plant = plant == "no_packed_tail" ? 11 : plant == "mip_pitch" ? 12 : 0;
  assets::pac::Pac disc, dlc;
  if (!OpenPac(root + "\\extracted\\pac\\prtcls.pac", disc) || !OpenPac(root + "\\dlc\\DLC Pack 3\\pac\\prtcls_dlc04.pac", dlc)) { std::printf("FAIL  open the player's prtcls.pac / prtcls_dlc04.pac under %s\n", root.c_str()); return 2; }

  // the index, as BuildParticleSprites builds it (the disc only), and the published file sprites
  NameIndex idx;
  assets::particle_sprites::ForEachDirectory(disc, [&](const std::string& leaf, const uint8_t* d, size_t n) { idx.AddSpriteDirectory(d, n, "pac\\prtcls.pac", leaf); });
  auto files = std::make_shared<std::unordered_map<std::string, assets::Texture>>();
  idx.ForEach([&](uint64_t, const NameIndex::Name& nm) {
    assets::Texture t;
    std::string why;
    if (assets::particle_sprites::LoadSprite(disc, nm.entry, t, why)) (*files)[nm.entry] = std::move(t);
  });
  std::shared_ptr<const std::unordered_map<std::string, assets::Texture>> cfiles = files;
  std::printf("info: disc index %zu distinct pictures, %zu file sprites\n", idx.size(), files->size());

  assets::Texture glow, cloud, light;
  std::string why;
  const bool have = assets::particle_sprites::LoadSprite(dlc, "p0385/0/1/beam_glow00", glow, why) && assets::particle_sprites::LoadSprite(dlc, "p0385/50/1/Cloud06", cloud, why) &&
                    assets::particle_sprites::LoadSprite(disc, "p0100/110/1/Light0001", light, why);
  Check(have, "the player's files hold Fandango's beam_glow00 / Cloud06 (prtcls_dlc04.pac) and the disc Light0001", why);
  if (!have) return 1;
  Check(!idx.Find(KeyOf(glow)) && !idx.Find(KeyOf(cloud)), "the DLC pictures are NOT in the disc index (the FAN run's not_in_index)");
  std::printf("info: beam_glow00 %ux%u mips %u key %016llX; Cloud06 %ux%u mips %u\n", glow.width, glow.height, glow.mips, (unsigned long long)KeyOf(glow), cloud.width, cloud.height, cloud.mips);

  // 1. the DLC pictures: named from the game's own texture, every level equal to the file's
  GuestSprites store;
  for (const assets::Texture* t : {&glow, &cloud}) {
    NamerStats st;
    const Named n = NameForged(*t, idx, store, &st);
    uint64_t key = 0;
    const bool guest_name = n.ok && n.pac == kGuestSpritePac && GuestSpriteKey(n.entry, key) && key == KeyOf(*t);
    const auto pic = store.Find(KeyOf(*t));
    const bool equal = pic && pic->width == t->width && pic->height == t->height && pic->format == t->format && pic->mips == t->mips && pic->data == t->data;
    char d[200];
    std::snprintf(d, sizeof d, "%s: named %d as %s|%s, guest %llu, guest_failed %llu; store picture mips %u, %zu bytes (file %u, %zu)", t == &glow ? "beam_glow00" : "Cloud06", n.ok ? 1 : 0, n.pac.c_str(), n.entry.c_str(),
                  (unsigned long long)st.guest, (unsigned long long)st.guest_failed, pic ? pic->mips : 0, pic ? pic->data.size() : size_t(0), t->mips, t->data.size());
    Check(guest_name && st.guest == 1 && st.guest_failed == 0, "a DLC picture the index misses is named from the game's own texture (guest:prtcls + its content key)", d);
    Check(equal, "the guest picture is the file's, every level byte for byte (mip chain with its packed tail)", d);
  }

  // 2. a disc picture: named by the index; the dev self-check reads the guest chain and finds it equal to the file sprite
  {
    NamerStats st;
    const Named n = NameForged(light, idx, store, &st, [&](const std::string& e) -> std::shared_ptr<const assets::Texture> {
      const auto it = cfiles->find(e);
      return it == cfiles->end() ? nullptr : std::shared_ptr<const assets::Texture>(cfiles, &it->second);
    });
    Check(n.ok && n.pac == "pac\\prtcls.pac" && st.guest == 0, "a disc picture is still named by the index (no guest read)", n.pac + "|" + n.entry);
    Check(st.guest_check_equal == 1 && st.guest_check_differ == 0, "the dev self-check: the guest chain of an index-named sprite equals the file's levels",
          "equal " + std::to_string(st.guest_check_equal) + ", differ " + std::to_string(st.guest_check_differ));
    // review (S108 PRT): a guest texture holding mip 0 only (header max level 0) compares one level; that must not count as `equal` (a whole-chain proof of the mip reader): it is `short`.
    // (prtcls_pac.md "Sprite textures in the draw stream": the GPU was handed ONE level for every captured particle image, so a live run may see exactly this.)
    assets::Texture light1 = light;
    light1.mips = 1;
    light1.data.resize(size_t((light.width + 3) / 4) * ((light.height + 3) / 4) * (light.format == assets::TexFormat::kBC1 ? 8 : 16));
    NamerStats st1;
    const Named n1 = NameForged(light1, idx, store, &st1, [&](const std::string& e) -> std::shared_ptr<const assets::Texture> {
      const auto it = cfiles->find(e);
      return it == cfiles->end() ? nullptr : std::shared_ptr<const assets::Texture>(cfiles, &it->second);
    });
    Check(n1.ok && st1.guest_check_equal == 0 && st1.guest_check_differ == 0 && st1.guest_check_short == 1, "the dev self-check: a guest holding mip 0 only is counted short, never equal",
          "equal " + std::to_string(st1.guest_check_equal) + ", differ " + std::to_string(st1.guest_check_differ) + ", short " + std::to_string(st1.guest_check_short));
  }

  // 3. the drawn batch: the real resolve (ParticleSpriteFor + MakeSpriteImage) and LiveParticles with skip-unnamed on
  {
    std::string err;
    rdr::RendererOptions ro;
    ro.validation = 1;
    auto renderer = rdr::Renderer::Create(ro, err);
    Check(renderer != nullptr, "a headless renderer", err);
    if (renderer) {
      FrontEnd fe(*renderer);
      GuestSprites draw_store;
      const Named n = NameForged(glow, idx, draw_store);
      uint8_t c[3];
      const LiveParticlesStats s = DrawBatch(*renderer, fe, n, cfiles, draw_store, c);
      char d[200];
      std::snprintf(d, sizeof d, "draws %llu, batches %llu, no_texture %llu, unnamed_skipped %llu, brightest pixel (%u %u %u)", (unsigned long long)s.draws, (unsigned long long)s.batches, (unsigned long long)s.no_texture,
                    (unsigned long long)s.unnamed_skipped, c[0], c[1], c[2]);
      Check(s.draws > 0 && s.unnamed_skipped == 0 && s.no_texture == 0 && int(c[0]) + c[1] + c[2] > 30, "Fandango's beam glow batch draws (the FAN run: draws 0, particles_unnamed_skipped 823)", d);
      Check(fe.validation_errors() == 0, "0 validation errors", std::to_string(fe.validation_errors()));
    }
  }

  // 4. the store's bound (count cap; the oldest goes) and the offline replay case (a guest name with no picture: the resolve says it is a guest name, frame_hooks.cpp logs it by key)
  {
    GuestSprites s;
    auto tiny = std::make_shared<assets::Texture>();
    tiny->data.assign(16, 0);
    for (uint64_t k = 1; k <= GuestSprites::kMaxEntries + 44; ++k) s.Put(k, tiny);
    Check(s.entries() == GuestSprites::kMaxEntries && s.evicted() == 44 && !s.Find(1) && s.Find(GuestSprites::kMaxEntries + 44), "the guest store is bounded (256 pictures / 32 MB), least recently named first",
          std::to_string(s.entries()) + " entries, evicted " + std::to_string(s.evicted()));
    GuestSprites empty;
    bool guest = false;
    const auto t = ParticleSpriteFor(kGuestSpritePac, GuestSpriteEntry(KeyOf(glow)), cfiles, empty, guest);
    Check(!t && guest, "a guest name with no picture in the process (a record replayed offline) resolves to nothing AND is known as a guest name (the rig's loud line)");
  }

  std::printf("prt_unit: %d passed, %d failed%s\n", g_pass, g_fail, plant.empty() ? "" : ("  (plant " + plant + ")").c_str());
  return g_fail ? 1 : 0;
}
