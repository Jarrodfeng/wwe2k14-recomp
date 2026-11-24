// S103 B1: the game's per-draw SAMPLER state, read from the player's own effect files at run time (docs\research\s103\T1_samplers.md, docs\formats\effect_techniques.md).
//
// The 360 has no sampler objects: a sampler's state is bits in the stage's fetch record, set by state calls. For the model draws the calls come from the effect PASS's
// sampler block (pass+16, applied by sub_82BAE370 op 6): one {u16 stage, u16 state offset, u32 value} entry per state, the same in every pass of an effect for a stage
// (census: 0 conflicts). This file is the C++ twin of tools\shaders\effect_table.py `sampler_block` / `sampler_table`. The table is BUILT AT RUN TIME from the player's
// pac\_startup.pac (live_assets.cpp) and never stored or committed; only the layout and the named code tables below are ours.
//
// Sampler block (body-relative offset at pass+16) [C listing 0x82BAE5B0-0x82BAE6D4]:
//   +0..+127 eight 16-byte "touched state" masks;  +128 / +132 / +136 the counts of lists A (int parameter) / B (float parameter) / C (immediate);
//   entries from +140, 8 bytes each, list A then B then C. The census holds only list C. Value = the device's own argument (device[468 + offset](device, stage, value)).
// State offsets: 0 U, 4 V, 8 W (Xenos clamp mode: 0 wrap, 1 mirror, 2 clamp to last texel, 6 clamp to border) [sub_82CAAFD0]; 16 / 20 = min / mag filter,
// 24 = mip filter (0 point, 1 linear, 2 base map; >= 4 anisotropic flag) [sub_82CAAA00, sub_82CAABA0]; 12 border colour; 28 LOD bias.
// (T1 1.1 labels 16 / 20 as min / mag; the census has min == mag == 1 everywhere, so the order cannot matter for what the files hold.)
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "read_rule.h"

namespace rdr_scene::sampler_table {

// ---- Our own, named code tables (the game addresses are the sources) ------------------------------------------------------------------------------------------
// Wrapper call codes, vtable slot 28 (pixel address, sub_82C23D78 @ 0x8206C740) and slot 30 (pixel filter, sub_82C23FE0 @ 0x8206C748) of the command recorder
// (vtable 0x8206C6D0, built by sub_82C13A80); the vertex-stage twins are slot 27 sub_82C23C60 (address) and slot 29 sub_82C23E98 (filter). Each records op 4
// (sub_82C26E00: stage, state offset, value). The callers: the model draw's per-parameter override (sub_82C04DF0), particles (sub_82A0D0E0 / sub_82A0DAC8), UI.
struct Addr { uint8_t u, v; bool set; };
inline Addr AddrFromCode(int code) {   // sub_82C23D78 [C listing]
  switch (code) {
    case 0: return {0, 0, true};
    case 1: return {1, 1, true};
    case 2: return {2, 2, true};
    case 3: return {0, 0, false};   // writes nothing
    case 4: return {6, 6, true};
    case 5: return {0, 1, true};
    case 6: return {1, 0, true};
    case 7: return {2, 2, true};
    default: return {0, 0, false};
  }
}
struct Filter { uint8_t min, mag, mip; bool set; };   // device filter arguments: 0 point, 1 linear, 2 base map, 4 anisotropic flag
inline Filter FilterFromCode(int code) {   // sub_82C23FE0 [C listing]
  switch (code) {
    case 0: return {2, 2, 2, true};
    case 1: return {0, 0, 0, true};
    case 2: return {1, 1, 1, true};   // what every textured model draw uses: trilinear
    case 3: return {4, 4, 4, true};
    case 6: return {2, 2, 2, true};
    default: return {0, 0, 0, false};
  }
}
// The model draw's d+28 override (sub_82C04DF0: 13 near-identical blocks, e.g. 0x82C05772-0x82C057B8): d+28 == 0 -> [model+524] (0 in the constructors read), 1 -> 0, 2 -> 2.
inline int AddrCodeOfDescriptor(uint32_t d28, int model524 = 0) { return d28 == 0 ? model524 : d28 == 1 ? 0 : d28 == 2 ? 2 : -1; }
// Particle sprite byte b (0..3) -> wrapper code {2, 5, 6, 1} (sub_82A0D0E0 / sub_82A0DAC8; docs\formats\particle_items.md). ParticleBatch::addr = b + 1 (0 = not set).
inline int ParticleAddrCode(unsigned b) { static const int k[4] = {2, 5, 6, 1}; return k[b & 3]; }

// Xenos clamp mode (device value) -> VkSamplerAddressMode (reader.h AddressMode, S4): 0 repeat, 1 mirrored, 2 / 4 clamp to edge, 3 / 5 / 7 mirror-clamp, 6 clamp to border.
inline uint8_t VkAddress(unsigned m) {
  switch (m) {
    case 0: return 0;
    case 1: return 1;
    case 2: case 4: return 2;
    case 3: case 5: case 7: return 4;
    case 6: return 3;
    default: return 2;
  }
}

// ---- The table ------------------------------------------------------------------------------------------------------------------------------------------------
constexpr int kStages = 32;   // pixel 0..15, vertex 16 + i
struct Stage {
  bool set = false;                  // the effect's sampler block names this stage
  uint8_t seen = 0;                  // bit (state offset / 4) per state already read
  uint8_t u = 0, v = 0, w = 0;       // Xenos clamp modes
  uint8_t mag = 0, min = 0, mip = 2; // device filter args (the device's defaults: point / point / base map)
};
struct EffectSamplers { std::array<Stage, kStages> st{}; bool any = false; };
using Table = std::unordered_map<std::string, EffectSamplers>;   // key: the lowercased effect name

// One effect blob (3-word header + body) -> samplers (tools\shaders\effect_table.py parse_effect + sampler_block). Returns the number of pass-entry conflicts
// (a stage given two different values by different passes; the first wins; the census has none).
inline uint32_t ParseEffect(const uint8_t* blob, size_t blob_n, EffectSamplers& out) {
  using read_rule::Be32;
  uint32_t conflicts = 0;
  if (blob_n < 12 || uint64_t(Be32(blob + 4)) + 12 > blob_n) return 0;
  const uint8_t* b = blob + 12;
  const size_t n = Be32(blob + 4);
  if (n < 0x210) return 0;
  const uint32_t tab = Be32(b + 0x200), cnt = Be32(b + 0x20C);
  if (!(cnt > 0 && cnt < 4096 && uint64_t(tab) + 4ull * cnt <= n)) return 0;
  for (uint32_t ti = 0; ti < cnt; ++ti) {
    const uint32_t t = Be32(b + tab + 4 * ti);
    if (uint64_t(t) + 20 > n) continue;
    const uint32_t npass = Be32(b + t + 4);
    if (!(npass > 0 && npass < 64) || uint64_t(t) + 16 + 4ull * npass > n) continue;
    for (uint32_t p = 0; p < npass; ++p) {
      const uint32_t ps = Be32(b + t + 16 + 4 * p);
      if (uint64_t(ps) + 20 > n) continue;
      const uint32_t off = Be32(b + ps + 16);
      if (off == 0 || uint64_t(off) + 140 > n) continue;
      const uint32_t ca = Be32(b + off + 128), cb = Be32(b + off + 132), cc = Be32(b + off + 136);
      const uint64_t total = uint64_t(ca) + cb + cc;   // (a forged block cannot wrap the sum: S103 B1 review)
      if (total > 512 || uint64_t(off) + 140 + 8ull * total > n) continue;
      for (uint64_t i = uint64_t(ca) + cb; i < total; ++i) {   // list C only: the immediates (lists A and B are empty in every effect file seen)
        const uint8_t* e = b + off + 140 + 8 * size_t(i);
        const unsigned stage = unsigned(e[0]) << 8 | e[1], state = unsigned(e[2]) << 8 | e[3];
        const uint32_t val = Be32(e + 4);
        if (stage >= kStages) continue;
        Stage& s = out.st[stage];
        uint8_t* field = nullptr;
        switch (state) {
          case 0: field = &s.u; break;
          case 4: field = &s.v; break;
          case 8: field = &s.w; break;
          case 16: field = &s.min; break;
          case 20: field = &s.mag; break;
          case 24: field = &s.mip; break;
          default: break;   // 12 border, 28 bias, 56: never set by the effects seen
        }
        if (!field) continue;
        const uint8_t bit = uint8_t(1u << (state / 4 > 6 ? 7 : state / 4));
        if ((s.seen & bit) && *field != uint8_t(val)) ++conflicts;   // a later pass disagrees: the first wins
        if (!(s.seen & bit)) *field = uint8_t(val);
        s.seen |= bit;
        s.set = true;
        out.any = true;
      }
    }
  }
  return conflicts;
}

// pac\_startup.pac key 0 (decoded): the effect leaf (read_rule.h ParseEffectLeaf's layout). `conflicts` accumulates ParseEffect's.
inline bool ParseLeaf(const uint8_t* d, size_t n, Table& out, uint32_t* conflicts = nullptr) {
  using read_rule::Le32;
  if (n < 16) return false;
  const uint32_t count = Le32(d);
  if (count == 0 || count > 100000 || 16 + 32ull * count > n) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = d + 16 + 32 * size_t(i);
    if (std::memcmp(e + 16, "obj\0", 4) != 0) return false;
    const uint32_t size = Le32(e + 20), off = Le32(e + 24);
    if (uint64_t(off) + size > n) continue;
    size_t len = 0;
    while (len < 16 && e[len]) ++len;
    EffectSamplers es;
    const uint32_t c = ParseEffect(d + off, size, es);
    if (conflicts) *conflicts += c;
    out.emplace(read_rule::Lower(std::string(reinterpret_cast<const char*>(e), len)), std::move(es));   // first one wins (as read_rule / effect_table.py)
  }
  return !out.empty();
}

inline std::mutex& Mutex() { static std::mutex mu; return mu; }
inline std::shared_ptr<const Table>& Slot() { static std::shared_ptr<const Table> t; return t; }
inline void Publish(std::shared_ptr<const Table> t) {
  std::lock_guard<std::mutex> l(Mutex());
  static std::vector<std::shared_ptr<const Table>> kept;   // a replaced table is never freed: pointers taken from it stay valid
  if (Slot()) kept.push_back(Slot());
  Slot() = std::move(t);
}
inline std::shared_ptr<const Table> Now() { std::lock_guard<std::mutex> l(Mutex()); return Slot(); }

// ---- Resolving one draw's sampler -----------------------------------------------------------------------------------------------------------------------------
// Texture filtering (the player's setting). anisotropic_override levels (the stream build's rule, reader.h:3726): -1 = the game's own (no anisotropy), 0..5 -> 1 / 1 / 2 / 4 / 8 / 16.
// Applies only where min and mag are linear and the mip filter is not point; the shadow stage (mip point) therefore stays 1x.
// PLAYER DEFAULT, decided by Jarrod (S103): STANDARD = the game's own filtering = no anisotropy (1x); higher levels follow the setting (reader.h rule).
// kAnisoFollowsSetting = false would restore the old fixed 8x on every eligible sampler (one line to flip).
inline constexpr bool kAnisoFollowsSetting = true;
inline constexpr int kAnisoFixed = 8;
inline std::atomic<int> g_aniso_override{-1};   // anisotropic_override; set by start_settings.cpp / gfx_menu.h
// Levels 2..5 -> 2 / 4 / 8 / 16; anything else (-1, 0, 1, and values past 5) -> 1x, as reader.h:3726 (a level outside 2..5 never reaches the shift).
inline uint8_t AnisoFor(int level) { return (level >= 2 && level <= 5) ? uint8_t(1u << (level - 1)) : uint8_t(1); }
// RIG-ONLY plant (set by the rig from WWE_RDR_PLANT_OLD_SAMPLERS; game builds never set it, no getenv in game code): brings back the old rules, no table and a fixed 8x everywhere.
inline bool g_plant_old_samplers = false;
// One relaxed atomic read per draw; the only per-draw cost of the setting. Flip mid-run through anisotropic_override (the menu's TEXTURE FILTERING row sets g_aniso_override at once).
inline uint8_t AnisoWanted() {
  if (g_plant_old_samplers) return uint8_t(kAnisoFixed);
  return kAnisoFollowsSetting ? AnisoFor(g_aniso_override.load(std::memory_order_relaxed)) : uint8_t(kAnisoFixed);
}

// The effect's stages, found once per bound material (front_end.cpp SamplerSlot). The table is published once per process and kept alive for it (Publish never frees one),
// so a pointer into it stays valid.
inline const EffectSamplers* Find(const std::string& effect) {
  const std::shared_ptr<const Table> t = Now();
  if (!t) return nullptr;
  const auto it = t->find(read_rule::Lower(effect));
  return it == t->end() ? nullptr : &it->second;
}

// Fill `sd` from a table stage. `map_slot`: the slot holds one of our own frame maps (the shadow / plane map: single-level): only there is the table's point mip taken; a parameter
// stage gets trilinear, as the model draw records it (sub_82C04DF0 calls the filter wrapper with code 2 after the pass). The d+28 address override is not in our draw data: the pass
// value stays (the compare tool's mismatch set is that data; none was seen).
template <class Desc>
inline void Apply(const Stage& s, Desc& sd, bool map_slot) {
  sd.address_u = VkAddress(s.u);
  sd.address_v = VkAddress(s.v);
  sd.address_w = VkAddress(s.w);
  sd.mag_filter = s.mag == 1;
  sd.min_filter = s.min == 1;
  sd.mip_mode = (s.mip == 1 || (s.mip == 0 && !map_slot)) ? 1 : 0;
  const bool linear = sd.mag_filter && sd.min_filter && sd.mip_mode == 1;   // a map slot at point mip stays 1x
  sd.max_anisotropy = linear ? AnisoWanted() : uint8_t(1);
  sd.max_lod = 1000.f;   // our images' own chain bounds it
}

// S110 ARMBAND (docs\research\s110\ARMBAND.md): the Superstar Threads editor's mask stages. Its draw callback sub_8339F0E0 binds Mask1..4 at pixel samplers
// 7 / 4 / 14 / 2 and after each bind calls the slot-28 wrapper with code 2 and the slot-30 wrapper with code 1 (+1E8 / +1F8, +284 / +294, +320 / +330,
// +3BC / +3CC): clamp / clamp, point / point / point. The model draw calls it after its parameter loop (sub_82C04DF0 +4A20), and the STS effects' pass
// blocks (edit.pac p9/0) name none of those stages, so this is the state the change-colour shader fetches the masks with. A linear fetch blends a channel
// into its neighbour's and the shader's first-non-zero test then hands a thin print's texels to the cloth channel. max_lod 0: the game's masks have one level.
template <class Desc>
inline void ChangeMaskSampler(Desc& sd) {
  const Addr a = AddrFromCode(2);
  const Filter f = FilterFromCode(1);
  sd.address_u = VkAddress(a.u);
  sd.address_v = VkAddress(a.v);
  sd.address_w = VkAddress(a.u);
  sd.mag_filter = f.mag == 1;
  sd.min_filter = f.min == 1;
  sd.mip_mode = f.mip == 1 ? 1 : 0;
  sd.max_anisotropy = 1;
  sd.max_lod = 0.f;
}

// S110 LOGOSAMP (docs\research\s110\LOGOSAMP.md): the Create a Superstar logo stages. Its draw callback sub_822E6CC8 binds "logo" at pixel sampler 7 and "logo2" at 4 and
// after each bind calls the slot-28 wrapper with code 2 and the slot-30 wrapper with code 2 (0x822E6D60 / 0x822E6D70, 0x822E6DCC / 0x822E6DDC): clamp / clamp, linear /
// linear / linear mip. The model draw calls it after its parameter loop (sub_82C04DF0 0x82C09810), and no logo effect of the editor's table (edit.pac p7/0) names
// stage 4, nor stage 7 but yCh_Ls. Trilinear: the player's level applies as on every trilinear stage (Apply's rule); our logo pictures are one level, max_lod as Apply.
template <class Desc>
inline void CawLogoSampler(Desc& sd) {
  const Addr a = AddrFromCode(2);
  const Filter f = FilterFromCode(2);
  sd.address_u = VkAddress(a.u);
  sd.address_v = VkAddress(a.v);
  sd.address_w = VkAddress(a.u);
  sd.mag_filter = f.mag == 1;
  sd.min_filter = f.min == 1;
  sd.mip_mode = f.mip == 1 ? 1 : 0;
  const bool linear = sd.mag_filter && sd.min_filter && sd.mip_mode == 1;
  sd.max_anisotropy = linear ? AnisoWanted() : uint8_t(1);
  sd.max_lod = 1000.f;
}

// S110 LOGOSAMP (review): the Superstar Threads editor's ENTRANCE PROP mask stage. Its draw callback sub_8338EC00 (installed only by the editor gear object's render node sub_8338ED08) binds the
// prop's mask ([G+108]; nothing when [G+100] is 0) at pixel sampler 12, then calls the slot-28 wrapper with (12, 2) and the slot-30 wrapper with (12, 2) (0x8338ECDC / 0x8338ECEC): clamp / clamp,
// linear / linear / linear mip = the logo callback's codes, so the same state (the player's level as on every trilinear stage).
template <class Desc>
inline void EditorPropMaskSampler(Desc& sd) { CawLogoSampler(sd); }

}  // namespace rdr_scene::sampler_table
