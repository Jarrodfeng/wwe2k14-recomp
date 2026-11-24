// S103 B1: the C++ twin check of tools\shaders\effect_table.py's sampler table. Reads decoded effect leaves (files written by sampler_compare.py --cpp under out\b1),
// parses them with src\renderer_scene\sampler_table.h (the shipped parser) and prints one line per (effect, stage): `effect stage u v w mag min mip`. Also checks the code tables
// (sub_82C23D78 / sub_82C23FE0 / the particle codes) against fixed expectations; prints `codes ok` or exits 1.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "../../src/renderer_scene/sampler_table.h"

int main(int argc, char** argv) {
  namespace st = rdr_scene::sampler_table;
  int rc = 0;
  const auto A = [&](int code, int u, int v, bool set) { const st::Addr a = st::AddrFromCode(code); if (a.set != set || (set && (a.u != u || a.v != v))) { std::printf("codes FAIL addr %d\n", code); rc = 1; } };
  A(0, 0, 0, true); A(1, 1, 1, true); A(2, 2, 2, true); A(3, 0, 0, false); A(4, 6, 6, true); A(5, 0, 1, true); A(6, 1, 0, true); A(7, 2, 2, true);
  const auto F = [&](int code, int mn, int mg, int mp) { const st::Filter f = st::FilterFromCode(code); if (!f.set || f.min != mn || f.mag != mg || f.mip != mp) { std::printf("codes FAIL filter %d\n", code); rc = 1; } };
  F(0, 2, 2, 2); F(1, 0, 0, 0); F(2, 1, 1, 1); F(3, 4, 4, 4); F(6, 2, 2, 2);
  // particle byte b -> code {2,5,6,1} -> (clamp/clamp, U wrap V mirror, U mirror V wrap, mirror/mirror)
  const uint8_t pu[4] = {2, 0, 1, 1}, pv[4] = {2, 1, 0, 1};
  for (unsigned b = 0; b < 4; ++b) { const st::Addr a = st::AddrFromCode(st::ParticleAddrCode(b)); if (a.u != pu[b] || a.v != pv[b]) { std::printf("codes FAIL particle %u\n", b); rc = 1; } }
  if (st::AddrCodeOfDescriptor(0, 0) != 0 || st::AddrCodeOfDescriptor(1) != 0 || st::AddrCodeOfDescriptor(2) != 2) { std::printf("codes FAIL d+28\n"); rc = 1; }
  if (st::AnisoFor(-1) != 1 || st::AnisoFor(0) != 1 || st::AnisoFor(1) != 1 || st::AnisoFor(2) != 2 || st::AnisoFor(3) != 4 || st::AnisoFor(4) != 8 || st::AnisoFor(5) != 16 || st::AnisoFor(6) != 1 || st::AnisoFor(9) != 1 || st::AnisoFor(40) != 1 || st::AnisoFor(-7) != 1) { std::printf("codes FAIL aniso\n"); rc = 1; }
  {  // point mip only on a map slot; a parameter stage is trilinear (S103 B1 review item 5); the player's level reaches only linear samplers
    struct D { uint8_t mag_filter, min_filter, mip_mode, address_u, address_v, address_w, max_anisotropy; float max_lod; } p{}, m{};
    st::Stage s; s.set = true; s.mag = s.min = 1; s.mip = 0;
    st::g_aniso_override.store(4);
    st::Apply(s, p, false); st::Apply(s, m, true);
    if (p.mip_mode != 1 || p.max_anisotropy != 8 || m.mip_mode != 0 || m.max_anisotropy != 1) { std::printf("codes FAIL point-mip rule\n"); rc = 1; }
    st::g_aniso_override.store(-1);
    st::Apply(s, p, false);
    if (p.max_anisotropy != 1) { std::printf("codes FAIL STANDARD is 1x\n"); rc = 1; }
  }
  {  // S110 ARMBAND: the Threads editor's mask stages (sub_8339F0E0: address code 2, filter code 1) = clamp, point / point / point, 1x, level 0, whatever the setting
    struct D { uint8_t mag_filter, min_filter, mip_mode, address_u, address_v, address_w, max_anisotropy; float max_lod; } d{1, 1, 1, 0, 0, 0, 8, 1000.f};
    st::g_aniso_override.store(4);
    st::ChangeMaskSampler(d);
    st::g_aniso_override.store(-1);
    if (d.mag_filter != 0 || d.min_filter != 0 || d.mip_mode != 0 || d.address_u != 2 || d.address_v != 2 || d.address_w != 2 || d.max_anisotropy != 1 || d.max_lod != 0.f) { std::printf("codes FAIL change-mask sampler\n"); rc = 1; }
  }
  {  // S110 LOGOSAMP: the Create a Superstar logo stages (sub_822E6CC8: address code 2, filter code 2) = clamp, trilinear; the player's level as on every trilinear stage; the image bounds the level
    struct D { uint8_t mag_filter, min_filter, mip_mode, address_u, address_v, address_w, max_anisotropy; float max_lod; } d{0, 0, 0, 0, 0, 0, 1, 0.f}, s{0, 0, 0, 0, 0, 0, 8, 0.f};
    st::g_aniso_override.store(4);
    st::CawLogoSampler(d);
    st::g_aniso_override.store(-1);
    st::CawLogoSampler(s);
    if (d.mag_filter != 1 || d.min_filter != 1 || d.mip_mode != 1 || d.address_u != 2 || d.address_v != 2 || d.address_w != 2 || d.max_anisotropy != 8 || d.max_lod != 1000.f || s.max_anisotropy != 1) { std::printf("codes FAIL caw-logo sampler\n"); rc = 1; }
  }
  {  // S110 LOGOSAMP (review): the Threads editor's prop mask stage (sub_8338EC00: address code 2, filter code 2) = the logo stages' state: clamp, trilinear, the player's level
    struct D { uint8_t mag_filter, min_filter, mip_mode, address_u, address_v, address_w, max_anisotropy; float max_lod; } d{0, 0, 0, 0, 0, 0, 1, 0.f};
    st::g_aniso_override.store(4);
    st::EditorPropMaskSampler(d);
    st::g_aniso_override.store(-1);
    if (d.mag_filter != 1 || d.min_filter != 1 || d.mip_mode != 1 || d.address_u != 2 || d.address_v != 2 || d.address_w != 2 || d.max_anisotropy != 8 || d.max_lod != 1000.f) { std::printf("codes FAIL editor-prop-mask sampler\n"); rc = 1; }
  }
  if (!rc) std::printf("codes ok\n");
  st::Table table;
  uint32_t conflicts = 0;
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    st::Table part;
    if (st::ParseLeaf(d.data(), d.size(), part, &conflicts)) for (auto& kv : part) table.emplace(kv.first, std::move(kv.second));
  }
  std::printf("conflicts %u 0 0 0 0 0 0\n", conflicts);
  for (const auto& kv : table)
    for (int s = 0; s < st::kStages; ++s) {
      const st::Stage& x = kv.second.st[size_t(s)];
      if (x.set) std::printf("%s %d %u %u %u %u %u %u\n", kv.first.c_str(), s, x.u, x.v, x.w, x.mag, x.min, x.mip);
    }
  return rc;
}
