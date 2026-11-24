// The camera the game HANDED to its renderer (S110 CUTCROWD, docs\research\s110\CUTCROWD.md; docs\systems\crowd.md Q6).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only and PURE (no game, SDK or renderer includes): inputs_core.h (ReadSceneCamera), crowd_latch_reader.h (the props' eye) and the units include it.
//
// WHY: the scene composer sub_823C8548 writes its output camera to the manager's copy [mgr + 1488] (view) / [mgr + 1552] (FOV, degrees) on every pass, but calls the hand-off sub_823C79A8 only when the last
// enabled camera's vcall +52 is 0 (@823C8910; a Camera01 motion start reads 1 for one pass). The hand-off flips [mgr+1360], writes the render view block B = mgr + 48 + 656 [mgr+1360] (view at B+240 from the slot's
// +0..63: loaded 0x823C79DC-0x823C7A2C, stored 0x823C7E20-0x823C7E2C; FOV in RADIANS at B+512 through sub_82BB3090 +28, f23 = fmuls f23,f0,f24 = slot+128 x [0x82004D70], listing wwe2k14_recomp.85.cpp:2448) and stores B at [[0x83929A68] + 256]
// (sub_82BA86B8 +50). Every crowd decision (sub_8250E110), the floor seats' screen test and the plane module read THAT block, so a record that reads +1488 draws the NEW shot of a cut with the OLD shot's crowd.
// The record therefore reads the block itself at the latch: no snapshot, no hook, nothing kept between frames, so nothing can go stale (a manager rebuilt at the same address included).
//   * view: B+240, the slot's 64 bytes as copied (byte-equal to +1488 on every pass the hand-off ran). The hand-off's +1424 effect (0x823C7A34-0x823C7D08, [mgr+1424] and [slot+184] != 0) is applied
//     before the store, so it is carried. Its MIRROR ([slot+176] != 0: row 0 negated, 0x823C7D0C-0x823C7D64) comes with a cull-winding flip (sub_82BB7C70 r4 = 1) the record cannot carry: a block whose 3x3
//     has a negative determinant is NOT taken (the live read, counted `mirror`).
//   * FOV: the block holds radians (r = fl(d k), k = [0x82004D70]). The degrees word is the live +1552 when it maps to r exactly (every pass the hand-off ran: byte-equal to the old record), else the degrees
//     derived from r (the nearest float d with fl(d k) == r: a gated pass whose FOV changed, or the +1424 effect).
//   * the block is taken only when B is one of the MAIN manager's two blocks (mgr + 48, mgr + 48 + 656). Not taken (the live read, counted): pointer 0 (no hand-off yet; sub_82BB2F70 zeroes it when the
//     current camera object is destroyed), another object's block (sub_82938FE8 points it at a sub-view and puts it back; another manager's hand-off), a mirror, or a block that fails the record's own tests
//     (rigid view, FOV in (1, 170)). With no manager at all (the CAW editor) ReadSceneCamera never gets here (reason 1, the draw thread's camera).
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace scene_inputs::handed {

inline constexpr uint32_t kMgrPtr = 0x836F3AA8, kLiveView = 1488, kLiveFov = 1552;               // the composer's copy (inputs_core.h kCamMgrPtr / kCamMgrView / kCamMgrFov)
inline constexpr uint32_t kEnginePtr = 0x83929A68, kEngineCam = 256;                             // [[0x83929A68] + 256] = the handed block (sub_82BA86B8 +50)
inline constexpr uint32_t kBlockBase = 48, kBlockStride = 656, kBlockView = 240, kBlockFovRad = 512;   // B = mgr + 48 + 656 k (sub_823C79A8 +434..+498); view +240, FOV radians +512 (sub_82BB3090 +28)
inline constexpr uint32_t kDegToRad = 0x82004D70;                                                // the image float the hand-off multiplies the slot's degrees by (0x823C7A00 lfs f24)
inline constexpr uint32_t kEffectWord = 1424, kSlotViews = 24, kSlotCount = 28, kSlotIndex = 32, kSlotStride = 272, kSlotMirror = 176, kSlotEffect = 184;   // census only (the reach leads)

// Test plants (the unit only): 1 = never the block (the old read of +1488 / +1552); 2 = any non-zero block taken (not only the main manager's); 3 = a mirrored block taken; 4 = the FOV always derived
// from the radians (never the live word); 5 = a block taken without the record's own tests.
inline int g_plant = 0;

enum Src : uint32_t { kSrcBlock = 0, kSrcNoBlock, kSrcOtherBlock, kSrcMirror, kSrcInvalid, kSrcCount };

// A rigid row-vector world -> view: the 3x3 rows orthonormal within 1e-3, column 3 = (0, 0, 0, 1) (zero and scaled matrices fail). (inputs_core.h IsRigidView is this function.)
inline bool IsRigidView(const float* v) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(v[i])) return false;
  if (std::fabs(v[3]) > 1e-3f || std::fabs(v[7]) > 1e-3f || std::fabs(v[11]) > 1e-3f || std::fabs(v[15] - 1.f) > 1e-3f) return false;
  for (int i = 0; i < 3; ++i)
    for (int j = i; j < 3; ++j) {
      const float d = v[i * 4] * v[j * 4] + v[i * 4 + 1] * v[j * 4 + 1] + v[i * 4 + 2] * v[j * 4 + 2];
      if (std::fabs(d - (i == j ? 1.f : 0.f)) > 1e-3f) return false;
    }
  return true;
}

inline float FloatOf(uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }
inline uint32_t WordOf(float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; }
// The hand-off's degrees -> radians: fmuls f23,f0,f24 (one rounding of the exact product).
inline uint32_t RadWordOf(uint32_t deg_w, uint32_t k_w) { return WordOf(float(double(FloatOf(deg_w)) * double(FloatOf(k_w)))); }
// The degrees word for a radians word: the first of d0 = fl(r / k), its upper and lower neighbour that maps back to r exactly; d0 when none does.
inline uint32_t DegWordFromRad(uint32_t rad_w, uint32_t k_w) {
  const float d0 = float(double(FloatOf(rad_w)) / double(FloatOf(k_w)));
  const float cand[3] = {d0, std::nextafter(d0, INFINITY), std::nextafter(d0, -INFINITY)};
  for (float c : cand)
    if (RadWordOf(WordOf(c), k_w) == rad_w) return WordOf(c);
  return WordOf(d0);
}

struct Pick {
  uint32_t src = kSrcNoBlock;
  uint32_t block = 0;          // the engine's block pointer as read (0 = none)
  float view[16] = {};         // kSrcBlock: the block's view (host floats)
  uint32_t fov_word = 0;       // kSrcBlock: the degrees word (host order)
  bool fov_derived = false;    // kSrcBlock: the degrees came from the radians (not the live word)
};

// The decision, pure. `word(guest, v)` reads one guest word in host order (false = unreadable); `mgr` = [0x836F3AA8] (non-zero). src != kSrcBlock = take the live +1488 / +1552.
template <class Word>
inline Pick PickCamera(Word&& word, uint32_t mgr) {
  Pick p;
  if (g_plant == 1) return p;
  uint32_t eng = 0, blk = 0;
  if (!word(kEnginePtr, eng) || !eng || !word(eng + kEngineCam, blk) || !blk) return p;
  p.block = blk;
  if (g_plant != 2 && blk != mgr + kBlockBase && blk != mgr + kBlockBase + kBlockStride) return p.src = kSrcOtherBlock, p;
  uint32_t w[16];
  for (uint32_t i = 0; i < 16; ++i)
    if (!word(blk + kBlockView + 4 * i, w[i])) return p.src = kSrcInvalid, p;
  for (int i = 0; i < 16; ++i) p.view[i] = FloatOf(w[i]);
  const float* v = p.view;
  const float det = v[0] * (v[5] * v[10] - v[6] * v[9]) - v[1] * (v[4] * v[10] - v[6] * v[8]) + v[2] * (v[4] * v[9] - v[5] * v[8]);
  if (std::isfinite(det) && det < 0.f && g_plant != 3) return p.src = kSrcMirror, p;
  uint32_t rad = 0, k = 0, live = 0;
  if (!word(blk + kBlockFovRad, rad) || !word(kDegToRad, k)) return p.src = kSrcInvalid, p;
  if (g_plant != 4 && word(mgr + kLiveFov, live) && RadWordOf(live, k) == rad) p.fov_word = live;
  else p.fov_word = DegWordFromRad(rad, k), p.fov_derived = true;
  const float fov = FloatOf(p.fov_word);
  if (g_plant != 5 && (!IsRigidView(v) || !(fov > 1.f && fov < 170.f))) return p.src = kSrcInvalid, p;
  p.src = kSrcBlock;
  return p;
}

// ---- census (inputs_contrib.h: once per latch on the latch thread; read by the census line on another thread)
struct Census {
  std::atomic<uint64_t> latches{0}, src[kSrcCount]{}, differ{0}, run{0}, longest{0}, fov_derived{0}, effect{0}, mirror_word{0}, effect_word{0};
};
inline Census& C() {
  static Census c;
  return c;
}
inline void ResetCensus() {
  Census& c = C();
  c.latches = 0, c.differ = 0, c.run = 0, c.longest = 0, c.fov_derived = 0, c.effect = 0, c.mirror_word = 0, c.effect_word = 0;
  for (auto& s : c.src) s = 0;
}
// One latch: `p` the decision, `differ` = the camera taken is not the live +1488 / +1552 bytes (a gated pass, the effect, a derived FOV); the reach leads: [mgr+1424] != 0, the composer slot's +176 / +184 != 0.
inline void NoteLatch(const Pick& p, bool differ, bool effect_1424, bool slot_176, bool slot_184) {
  Census& c = C();
  const auto inc = [](std::atomic<uint64_t>& a) { a.fetch_add(1, std::memory_order_relaxed); };
  inc(c.latches);
  inc(c.src[p.src < kSrcCount ? p.src : kSrcInvalid]);
  if (p.src == kSrcBlock && p.fov_derived) inc(c.fov_derived);
  if (differ) {
    inc(c.differ);
    const uint64_t r = c.run.fetch_add(1, std::memory_order_relaxed) + 1;
    if (r > c.longest.load(std::memory_order_relaxed)) c.longest.store(r, std::memory_order_relaxed);
  } else {
    c.run.store(0, std::memory_order_relaxed);
  }
  if (effect_1424) inc(c.effect);
  if (slot_176) inc(c.mirror_word);
  if (slot_184) inc(c.effect_word);
}
inline std::string CensusLine() {
  Census& c = C();
  const auto n = [](const std::atomic<uint64_t>& a) { return std::to_string((unsigned long long)a.load(std::memory_order_relaxed)); };
  return "handed: latches " + n(c.latches) + " block " + n(c.src[kSrcBlock]) + " differ " + n(c.differ) + " longest_differ " + n(c.longest) + " fov_derived " + n(c.fov_derived) +
         " live(no_block " + n(c.src[kSrcNoBlock]) + " other_block " + n(c.src[kSrcOtherBlock]) + " mirror " + n(c.src[kSrcMirror]) + " invalid " + n(c.src[kSrcInvalid]) + ")" +
         " reach(mgr1424 " + n(c.effect) + " slot176 " + n(c.mirror_word) + " slot184 " + n(c.effect_word) + ")";
}

}  // namespace scene_inputs::handed
