// S103 lane TX: the texture header words are CPU view addresses; the walk gives them in GPU form (+4 KB for the 0xE0000000 view), as the game's SetTexture does
// [C: listing 0x82CA8B6C-0x82CA8BEC; src\guest_view.h]. Synthetic textures only (no game files). Pure compile, 0 game runs.
//   texreg_unit            exit 0 when every check passes
//   texreg_unit --plant view_skipped   (g_tex_plant = 7: the walk leaves words 1 / 5 as stored) exit 0 only when the checks fail (see the plant)
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../../src/scene_live/particle_hasher.h"
#include "../../src/scene_live/particle_namer.h"

using namespace rdr_scene::live;
using namespace rdr_scene::live::particles;

static int g_fail = 0;
static void Check(bool ok, const char* what, const std::string& d = std::string()) {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, d.empty() ? "" : "  : ", d.c_str());
  if (!ok) ++g_fail;
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
struct Phys {   // physical memory: base -> bytes, addressed with the reader's own mask (TranslatePhysical: & 0x1FFFFFFF)
  std::map<uint32_t, std::vector<uint8_t>> r;
  bool Read(uint32_t a, uint32_t n, uint8_t* d) const {
    a &= 0x1FFFFFFFu;
    for (const auto& [b, v] : r)
      if (a >= b && uint64_t(a) + n <= uint64_t(b) + v.size()) return std::memcpy(d, v.data() + (a - b), n), true;
    return false;
  }
};
constexpr uint32_t kW = 32, kH = 32, kBx = kW / 4, kBy = kH / 4, kPitchBlocks = 32;
static std::vector<uint8_t> Linear(uint8_t seed) {
  std::vector<uint8_t> v(kBx * kBy * 8);
  for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(seed + 7 * i + (i >> 3));
  return v;
}
static std::vector<uint8_t> Tiled(const std::vector<uint8_t>& lin) {   // BC1, tiled, endian 2 (8-in-32): how the game stores it
  std::vector<uint8_t> mem(4096, 0);   // (one page: the decoy page must not overlap the true page)
  for (uint32_t y = 0; y < kBy; ++y)
    for (uint32_t x = 0; x < kBx; ++x) {
      uint8_t blk[8];
      std::memcpy(blk, lin.data() + (size_t(y) * kBx + x) * 8, 8);
      guest_tex::SwapBlock(blk, 8, 2);
      std::memcpy(mem.data() + guest_tex::TiledOffset2D(int32_t(x), int32_t(y), kPitchBlocks, 3), blk, 8);
    }
  return mem;
}
static uint64_t KeyOf(const std::vector<uint8_t>& lin) { return ContentKey(kW, kH, 0x31545844u, lin.data(), lin.size()); }
static void PutTexture(Mem& m, uint32_t id, uint32_t word1_base) {
  const uint32_t reg = 0x2000, entries = 0x3000, wrapper = 0x1000, header = 0x1800;
  m.w.clear();
  m.w[kTextureHandleGlobal] = reg, m.w[reg + 8] = entries, m.w[reg + 16] = entries + 8 * 0x10000u;
  m.w[entries + (id & 0xFFFF) * 8] = id, m.w[entries + (id & 0xFFFF) * 8 + 4] = wrapper;
  m.w[wrapper] = kWrapperVtableA, m.w[wrapper + kWrapperKind] = 0, m.w[wrapper + kWrapperTexture] = header;
  const uint32_t f[6] = {0x80000000u | (1u << 22), 18u | (2u << 6) | word1_base, (kW - 1) | ((kH - 1) << 13), 0, 0, 1u << 9};
  for (int i = 0; i < 6; ++i) m.w[header + kHeaderFetch + 4 * i] = f[i];
}
static std::string NameOf(const Mem& m, const Phys& px, const NameIndex& idx, uint32_t id) {
  GuestContentHasher<std::function<bool(uint32_t, uint32_t, uint8_t*)>> hs([&](uint32_t a, uint32_t n, uint8_t* d) { return px.Read(a, n, d); });
  HandleTextureNamer namer(m, hs, idx);
  std::string pac, ent;
  return namer.Name(id, 0, pac, ent) ? ent : std::string("<unnamed>");
}

int main(int argc, char** argv) {
  const bool g_tex_planted = argc > 2 && std::string(argv[1]) == "--plant" && std::string(argv[2]) == "view_skipped";
  if (g_tex_planted) guest_tex::g_tex_plant = 7;
  const auto lt = Linear(10), ld = Linear(90), la = Linear(170);
  NameIndex idx;
  idx.Add(KeyOf(lt), "pac", "true");
  idx.Add(KeyOf(ld), "pac", "decoy");
  idx.Add(KeyOf(la), "pac", "aview");
  Phys px;
  px.r[0x18E06000u] = Tiled(lt);   // the TRUE sprite: the E-view header word 0xF8E05000 is physical 0x18E06000
  px.r[0x18E05000u] = Tiled(ld);   // the page 4 KB early: a DIFFERENT index sprite (the S103 SM fault read here)
  px.r[0x18F00000u] = Tiled(la);
  Mem m;
  const uint32_t id = 0x091903BD;
  PutTexture(m, id, 0xF8E05000u);
  Check(NameOf(m, px, idx, id) == "true", "an E-view header word (0xF8E05000, pixels at physical 0x18E06000, a decoy at 0x18E05000) names the true sprite", NameOf(m, px, idx, id));
  PutTexture(m, id, 0xB8F00000u);   // A view: no +4 KB; physical 0x18F00000
  Check(NameOf(m, px, idx, id) == "aview", "an A-view header word (0xB8F00000) is not over-corrected", NameOf(m, px, idx, id));
  PutTexture(m, id, 0x18F00000u);   // already physical
  Check(NameOf(m, px, idx, id) == "aview", "an already physical header word is unchanged", NameOf(m, px, idx, id));
  TextureObject o;
  PutTexture(m, id, 0xF8E05000u);
  m.w[0x1800 + kHeaderFetch + 20] = (1u << 9) | 0xFC545000u;   // word 5 = dimension + the mip chain base (an E-view address)
  const bool ok = WalkTextureHandle(m, id, o) == TexWalk::Ok;
  Check(ok && o.fetch[1] == (0x18E06000u | 18u | (2u << 6)) && o.fetch[5] == (0x1C546000u | (1u << 9)), "the walk gives words 1 and 5 in GPU form and keeps their low bits", ok ? std::to_string(o.fetch[1]) : "walk failed");
  std::printf("texreg_unit: %d failed\n", g_fail);
  if (g_tex_planted) return g_fail ? 0 : 1;   // a planted twin: exit 0 only when the checks SEE the plant (same convention as p3b_unit)
  return g_fail ? 1 : 0;
}
