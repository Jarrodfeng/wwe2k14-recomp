// S103 KS1: the props light (light 1 of the shadow module) read at the latch from the module's light-1 record (the words sub_827E74F8 copies into R for light 1), so the floor's ShadowPrm
// does not depend on a model call that ran under that light (OURS skips the props pass). The formula is props_light_rows.h (PropsPrmFromWords); this file is the guest side: the module word, the
// page-checked reads, the planted wrong-offset twin. Header only; includes only inputs_core.h's ReadFn / ReadWord. Unit: tools\scene\ks1_unit.cpp over e5_fake_memory.h.
// Marks: [C] the module singleton 0x83924748 (sub_827E93D8 stores its `this` there, draw_camera_core.h kShadowModulePtr), the field offsets (listing sub_827E74F8, props_light_rows.h) and that it is the module the
// game's sub_827E95D8 gets as r3 (the Opus review re-derived the formula, every offset and the module word).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../renderer_scene/props_light_rows.h"
#include "inputs_core.h"

namespace scene_inputs {

inline constexpr uint32_t kPropsModulePtr = 0x83924748u, kPropsZeroWord = 0x8218B8CCu, kPropsOneWord = 0x8218B8C0u;
inline constexpr uint32_t kPropsRecOff = 1312;   // light-1 record: module + 1312 (the light-2 record is module + 304)

enum class PropsWhy : uint32_t { Ok = 0, NoModule, Unreadable, NonFinite, NoRange, Count };   // S103 PMC2 (was Bad): NonFinite = a non-finite ShadowPrm word / ViewProj row; NoRange = far <= near (no depth range)
inline bool PropsDegenerate(PropsWhy w) { return w == PropsWhy::NonFinite || w == PropsWhy::NoRange; }   // the light is in a degenerate state (the game's own, at a scene start)

inline PropsWhy ReadPropsLight(ReadFn read, uint64_t record_frame, rdr_scene::PropsLight& out) {
  using namespace rdr_scene;
  uint32_t mod = 0;
  if (!read || !ReadWord(read, kPropsModulePtr, mod)) return PropsWhy::Unreadable;
  if (mod < 0x10000) return PropsWhy::NoModule;
  const bool light2 = g_props_light_plant == 1;   // planted wrong offset: the light-2 branch's words (+1296 / +1300; 1120 from +204, 1140 from +224, 1144 from +228)
  auto fl = [](uint32_t be) { const uint32_t w = Swap32(be); float v; std::memcpy(&v, &w, 4); return v; };
  uint32_t blk[17];   // module + 192 .. +259 in one page-checked read (68 bytes), then 260 separately
  uint32_t w260 = 0, fz = 0, fo = 0, far_w = 0, near_w = 0, vp[16];
  const uint32_t far_off = light2 ? 1296u : 2304u, near_off = light2 ? 1300u : 2308u;
  if (!read(mod + 192, blk, sizeof blk) || !read(mod + 260, &w260, 4) || !read(mod + far_off, &far_w, 4) || !read(mod + near_off, &near_w, 4) || !read(kPropsZeroWord, &fz, 4) || !read(kPropsOneWord, &fo, 4) || !read(mod + (light2 ? 1104u : kPropsVpOff), vp, sizeof vp))
    return PropsWhy::Unreadable;
  auto at = [&](uint32_t off) { return fl(blk[(off - 192) / 4]); };
  PropsLightWords w;
  w.w192 = at(192);
  w.w200 = at(light2 ? 204 : 200);
  w.w212 = at(212);
  w.w216 = at(light2 ? 224 : 216);
  w.w220 = at(light2 ? 228 : 220);
  w.w260 = fl(w260);
  w.far_z = fl(far_w), w.near_z = fl(near_w);
  w.zero = fl(fz), w.one = fl(fo);
  PropsLight pl;
  pl.frame = uint32_t(record_frame);
  PropsPrmFromWords(w, pl.prm);
  for (int i = 0; i < 12; ++i)
    if (!std::isfinite(pl.prm[i / 4][i % 4])) return PropsWhy::NonFinite;
  if (!(pl.prm[0][1] > pl.prm[0][0])) return PropsWhy::NoRange;   // far <= near: no depth range (the receiver divides by it)
  for (int k = 0; k < 4; ++k) {   // ViewProj register row 3 = column 3 of M(R+912): m[4k + 3]
    pl.vp_row3[k] = fl(vp[4 * k + 3]);
    if (!std::isfinite(pl.vp_row3[k])) return PropsWhy::NonFinite;
  }
  pl.flags = kPlRead | kPlHaveVp;
  out = pl;
  return PropsWhy::Ok;
}

}  // namespace scene_inputs
