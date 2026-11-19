// S103 KS1: the props map's light (light 1 of the shadow module), read DIRECTLY from the module's light-1 record, so the floor's ShadowPrm (PS c168-c170) exists under OURS, where the game's props pass
// never runs and no model call ever sees the props light in R. Pure: no renderer, no game, no SDK. Record section 65 (kScPropsLight); an older record has none and PrmFromRecord falls back to the state search.
//
// Source [C listing sub_827E74F8, the light-1 branch at 0x827E76F8-0x827E7820, r30 = the module, r29 = module + 1312; q decomp sub_827E7DF0 +FC passes r4 = 0 for light 1 (record module + 1312)]:
//   d    = [m+2304] - [m+2308]; f30 = d == 0 ? zero : one / d        (float32; zero = word 0x8218B8CC, one = word 0x8218B8C0)
//   R+1108 = [m+2308] (near)    R+1112 = [m+2304] (far)    R+1104 = [m+192] * f30    R+1120 = [m+200]
//   R+1128 = [m+260]            R+1136 = [m+212]           R+1140 = t == 0 ? zero : one / t, t = [m+216] * f30     R+1144 = [m+220]
//   R+1148, +1152, +1156, +1160 = zero (fmr f1,f31 before each setter; the last through sub_82BCD138)
// LightRows / CasterRows (light_state_rows.h) take ShadowPrm = {F(1108), F(1112), F(1104), F(1120)}, {F(1128), F(1136), F(1140), F(1144)}, {F(1148), F(1152), F(1156), F(1160)}, so the same three rows come out here.
// The light-2 branch (0x827E7574) reads the same fields at +1296 / +1300 and +204 / +224 / +228 / +232 / +236 / +240: a reader on those words is the wrong light (the planted twin).
#pragma once
#include <cmath>
#include <cstdint>

namespace rdr_scene {

inline constexpr uint32_t kScPropsLight = 65;   // PropsLight[] (0 or 1 per frame). Optional (60-64 are the weapon / guest-model lanes'; 58 = Quad2D)
inline constexpr uint32_t kPropsLightLayout = 1;
enum PropsLightFlags : uint32_t { kPlRead = 1, kPlHaveVp = 2, kPlDegenerate = 4 };   // kPlHaveVp: vp_row3 was read. S103 PMC2: kPlDegenerate = a row with NO usable light (the latch found the game's light non-finite / without a depth range: the game's props pass draws nothing that frame); never read as a ShadowPrm (PrmFromRecord needs kPlRead | kPlHaveVp)
struct PropsLight {
  uint32_t version = kPropsLightLayout;
  uint32_t frame = 0;       // the record frame the latch read the module at
  uint32_t flags = 0;       // PropsLightFlags
  uint32_t pad = 0;
  float prm[3][4] = {};     // PS c168-c170
  float vp_row3[4] = {};    // light 1's ViewProj register row 3 = the words module+2112 + 12 / 28 / 44 / 60 (M(R+912) of the light-1 branch, sub_82BCCDD8 r4 = module + 1312 + 800; BuildCasterRows view_proj[3][k] = m[4k+3]): pairs the row with the circles
};
static_assert(sizeof(PropsLight) == 16 + 48 + 16, "PropsLight is a flat POD table");

// The module words the light-1 branch reads (host floats), and the two image constants.
struct PropsLightWords {
  float w192 = 0, w200 = 0, w212 = 0, w216 = 0, w220 = 0, w260 = 0;   // module + 192 / 200 / 212 / 216 / 220 / 260
  float far_z = 0, near_z = 0;                                         // module + 2304 (R+1112) / module + 2308 (R+1108)
  float zero = 0, one = 1;                                             // 0x8218B8CC / 0x8218B8C0
};

// kPropsVpOff: M(R+912) of light 1 sits at module + 2112 (listing 0x827E774C: addi r4,r29,800 with r29 = module + 1312).
inline constexpr uint32_t kPropsVpOff = 2112;

// Test-only planted faults (tools\scene\ks1_unit.cpp): 2 = no_scale (f30 = one), 3 = row_swap (rows 0 and 1 exchanged); 1 (the light-2 words) is applied by the reader. Never set outside a proof.
inline int g_props_light_plant = 0;

inline void PropsPrmFromWords(const PropsLightWords& w, float prm[3][4]) {
  volatile float d = w.far_z - w.near_z;   // volatile: float32 rounding at each operation (fsubs / fmuls / fdivs)
  float f30 = d == w.zero ? w.zero : w.one / d;
  if (g_props_light_plant == 2) f30 = w.one;
  volatile float a = w.w192 * f30;
  volatile float t = w.w216 * f30;
  const float inv = t == w.zero ? w.zero : w.one / t;
  const float r0[4] = {w.near_z, w.far_z, a, w.w200};
  const float r1[4] = {w.w260, w.w212, inv, w.w220};
  const int i0 = g_props_light_plant == 3 ? 1 : 0, i1 = g_props_light_plant == 3 ? 0 : 1;
  for (int k = 0; k < 4; ++k) {
    prm[i0][k] = r0[k];
    prm[i1][k] = r1[k];
    prm[2][k] = w.zero;
  }
}

}  // namespace rdr_scene
