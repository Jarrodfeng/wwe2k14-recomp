// The plane shadow's EyePos and PlanePos, recomputed at the latch (Session 103 lane E5; docs\renderer\LIVE_MAPS.md "Lane E5"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only; includes only our own pure headers (inputs_core.h's ReadFn / ReadWord and the record). The contributor (inputs_contrib.h), the offline driver (tools\scene\maps_draw.cpp) and the
// unit (tools\scene\e5_unit.cpp) run THESE functions. Marks: [C] read in a listing (cited: `py tools\analysis\list_function.py ADDR`), [L] a reading, [U] unknown.
//
// What the game does [C, listing sub_827ED6D0 0x827ED6D0-0x827ED958, called as sub_827ED6D0(module, kind) from the proxy loop of sub_827EED58 at 0x827EEE2C for kinds 1, 4, 5, 6, 7 (0, 2, 3 are skipped
// at 0x827EEE08-0x827EEE18)]:
//   vector v = the 16 bytes at module + 768 + 16 kind          (0x827ED6E0-0x827ED708: r11 = (kind + 48) * 16 + module, ld 0(r11) / 8(r11))
//   angle a  = the float at module + 896 + 4 kind               (0x827ED6E4-0x827ED70C: r9 = (kind + 224) * 4)
//   theta    = a * K0 - K1                                      (0x827ED710-0x827ED718: fmsubs f1 = f12 * [0x82004D70] - [0x82118560]; K0 = pi / 180 = 0.017453292, K1 = pi: image words 0x3C8EFA35 / 0x40490FDB)
//   v'       = v rotated about y by sub_82C329A8(&v, theta) (0x827ED724): x' = x cos + z sin, z' = z cos - x sin, y and w unchanged (listing 0x82C329D8-0x82C329F4: f30 = sub_822687C8 = cos,
//              f1 = sub_822B4C60 = sin [db: cos_f32_range_reduce_taylor_vmx / sinf_range_reduced_taylor_x23]; stfs f9 -> 0(r31) = x', fmsubs f10 -> 8(r31) = z')
//   the eye  = (x', y, z') stored at module + 992 / 996 / 1000   (0x827ED89C-0x827ED8A4: stfs f8,992 / f7,996 / f6,1000 from the rotated vector; the previous builder could not see this store).
//              Those words are the translation row of the 4x4 at module + 944 (identity + eye, then rotated by sub_82C335A0 / 82C33550 / 82C335F0 and inverted into +288 / +352 for the proxy's matrices).
// The tables are written by sub_827F19C0 (post_preset_load_by_arena, called when an arena's preset loads) from the arena table `[[0x83924768] + 48] + 24 [arena id]` as int -> float (0x827F1F10-0x827F22B4):
//   kind 0 +40 +44 +48 / angle +52; kind 1 +56 +60 +64 / angle +68; kind 4 +104 +108 +112 / angle +116; kind 5 +120 +124 +128 / angle +132; kind 6 +136 +140 +144 / angle +148; kind 7 +152 +156 +160 / angle +164
//   (offsets into the arena table entry), w = 1.0 ([0x8218B8C0]) in every vector's 4th word. So `w == 1.0` is the "this kind's table has been loaded" mark: a module whose preset has not loaded holds 0.
// PlanePos (VS c7) = (0, [module + 1440], 0, 0): sub_827EE190 0x827EE220-0x827EE234 builds it per proxy from [module+1440] and the 0.0 at 0x8218B8CC; [module+1440] is stored every update by sub_827EE380
// (0x827EE464-0x827EE470: lfs [0x8201BA24] = -120.0 -> stfs 1420(module + 20)), under the same gate as the plane frame (module+40 != 0 and word 0x836F39A0 == 0).
// Not claimed: that c72 is uploaded from module+992 [U: the uploader of c72 is not in the walk's listing (shadows.md 6.3)]. What is [C]: the stream's EyePos values (brock_losswin f73 / f110 / f146: the
// wrestler proxy (-450, -1400, 0), the ring (0,-800,1000) (-1000,-800,0) (0,-800,-1000) (1000,-800,0)) are exactly what this recomputation gives for a table of one base vector and the angles 0/90/180/270 (e5_unit).
//
// Precision: the game's sin / cos are range-reduced Taylor series in float; ours are the library's. They differ by <= 2e-7 relative, so an eye coordinate of magnitude 1400 differs by < 4e-4; the proof's bar is 0.01.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "inputs_core.h"

namespace scene_inputs {

inline constexpr uint32_t kPeVecOff = 768, kPeAngleOff = 896, kPePlanePosOff = 1440;
inline constexpr uint32_t kPeKindMask = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);   // the kinds the proxy loop visits
inline constexpr float kPeK0 = 0.0174532924f, kPeK1 = 3.14159274f;                                  // image words 0x3C8EFA35 / 0x40490FDB

// Test-only planted faults (tools\scene\e5_unit.cpp, maps_draw --eye-plant): 0 = none, 1 = no_pi (theta = a * K0, the -pi lost), 2 = rot_sign (the rotation turned the other way), 3 = kind_slot (the
// tables read one kind too high), 4 = degrees (the angle used as radians), 5 = stale (the first result is remembered and returned for ever), 6 = no_loaded_mark (the w == 1 test dropped). Never set outside a proof: one load per read.
inline int g_eye_plant = 0;

// theta = fmsubs(a, K0, K1): one rounding of a * K0 - K1 (the PPC fused op).
inline float EyeTheta(float angle) {
  if (g_eye_plant == 1) return angle * kPeK0;
  if (g_eye_plant == 4) return angle - kPeK1;
  return std::fmaf(angle, kPeK0, -kPeK1);
}

// The recomputation of one kind from its table words (host floats). x' = fmadds(x, cos, z * sin), z' = fmsubs(z, cos, x * sin): the products x * sin and z * sin are rounded singles, as fmuls does.
inline void EyeOfTable(const float vec[4], float angle, float out[4]) {
  const float th = EyeTheta(angle);
  const float cs = float(std::cos(double(th))), sn = float(std::sin(double(th)));
  const float sg = g_eye_plant == 2 ? -1.f : 1.f;
  const float zs = sg * vec[2] * sn, xs = sg * vec[0] * sn;
  out[0] = std::fmaf(vec[0], cs, zs);
  out[1] = vec[1];
  out[2] = std::fmaf(vec[2], cs, -xs);
  out[3] = vec[3];
}

enum class EyeWhy : uint32_t { Ok = 0, NoModule, Unreadable, NoneLoaded, Count };   // NoneLoaded: the module exists but no kind's table is loaded (the preset has not run / this arena has no plane)

// Reads the module's tables and recomputes the eyes. `pe` is filled only on Ok: have = the kinds whose table is loaded (w == 1.0, finite), eye[] their eyes, plane_pos = (0, [module+1440], 0, 0) when
// readable and finite (flags kPeHavePos). Returns Ok when at least one kind is loaded. Liveness: this reads the CURRENT module words; nothing is cached, so a freed module (word 0x83924758 = 0) says NoModule.
// S103 L9: `record_frame` = the frame the record carries (the plane frame's clock), NOT the hook frame.
inline EyeWhy ReadPlaneEyes(ReadFn read, uint64_t record_frame, rdr_scene::PlaneEyes& pe) {
  using namespace rdr_scene;
  uint32_t mod = 0;
  if (!read || !ReadWord(read, kPlaneModulePtr, mod)) return EyeWhy::Unreadable;
  if (mod < 0x10000) return EyeWhy::NoModule;
  PlaneEyes out;
  out.frame = uint32_t(record_frame);
  auto fl = [](uint32_t be) { const uint32_t w = Swap32(be); float v; std::memcpy(&v, &w, 4); return v; };
  for (uint32_t k = 0; k < kPeKinds; ++k) {
    if (!(kPeKindMask & (1u << k))) continue;
    uint32_t v[4], a = 0;
    const uint32_t kk = g_eye_plant == 3 ? k + 1 : k;
    if (!read(mod + kPeVecOff + 16u * kk, v, sizeof v) || !read(mod + kPeAngleOff + 4u * kk, &a, 4)) return EyeWhy::Unreadable;
    const float vec[4] = {fl(v[0]), fl(v[1]), fl(v[2]), fl(v[3])};
    const float ang = fl(a);
    if ((g_eye_plant != 6 && vec[3] != 1.0f) || !std::isfinite(vec[0]) || !std::isfinite(vec[1]) || !std::isfinite(vec[2]) || !std::isfinite(ang)) continue;   // not loaded (w is 0 until sub_827F19C0 ran) or garbage: no bit, no guess
    EyeOfTable(vec, ang, out.eye[k]);
    if (!std::isfinite(out.eye[k][0]) || !std::isfinite(out.eye[k][2])) continue;
    out.have |= 1u << k;
  }
  uint32_t py = 0;
  if (read(mod + kPePlanePosOff, &py, 4)) {   // raw big-endian word (fl swaps)
    const float y = fl(py);
    if (std::isfinite(y) && y != 0.f) out.plane_pos[1] = y, out.flags |= kPeHavePos;   // 0 = sub_827EE380 has not stored it yet (the stored constant is -120)
  }
  if (out.have == 0) return EyeWhy::NoneLoaded;
  if (g_eye_plant == 5) {   // the planted stale reader
    static PlaneEyes cached;
    static bool have_cached = false;
    if (have_cached) out = cached, out.frame = uint32_t(record_frame);
    else cached = out, have_cached = true;
  }
  pe = out;
  return EyeWhy::Ok;
}

}  // namespace scene_inputs
