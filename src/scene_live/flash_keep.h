// S111 FLASHCAM review (docs\research\s111\FLASHCAM.md, Review): the game's own keep rule of a crowd flash, CENSUS ONLY. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header only, no game, SDK or renderer-adapter includes (the guest is read through flash_fx_core.h's ReadFn): the flash writer (flash_fx_contrib.h FlashFxContributor::Take) and the offline unit
// (tools\scene\flash_camera_unit.cpp) run THESE functions. Nothing here places, hides or drops a flash: the answer rides the row (kFxfKeepCensus) to the after-post draw's census line.
//
// What the game does [C, decomp sub_823B2470, the visibility test the flash draw sub_823B27F0 runs while the item's +48 is set]:
//  * The camera is the walk thread's slot camera (sub_82BA7748), which the walk's begin sub_82BB4F00 installs from the engine's handed block [[0x83929A68] + 256]: its +240 is the VIEW (the point times +240
//    gives the view z, +D4..+E8) and +176 the PROJECTION (its row 1 negated, +100..+140); the clip matrix is +240 . (+176 with row 1 negated) (sub_82296630 at +144).
//  * The item point is +16..+24 with w = 1 (+11C). Refused when view z <= 50 (+E8). At phase word [0x836F3940] == 4 only the depth is tested (+F8).
//  * Screen point px = int(W/2 * (x/w + 1)), py = int(H/2 * (y/w + 1)), W / H = the ENGINE's words [[0x83929A68] + 300] / [+304] (+184..+1EC); refused unless 64 <= px <= W - 64 and 64 <= py <= H - 64 (+1F4..+218).
//  * The character box (+228..+368): for each of the 9 words at 0x836F38D0 (stride 4) whose object passes sub_823CBC50 (non-null, +1208, +224 and +232 non-zero), the point [[ch + 812] + 4] + 48..56 (w 1)
//    under the same clip matrix gives cpx = int(W/2 * x/w + W/2), cpy likewise; when 0 <= cpx <= W the flash is refused if cpx - 100 < px < cpx + 100 and cpy - 200 < py < cpy + 200 (the margins are 0, so
//    nothing is refused, when the word [0x836F3A94] is non-zero).
// At the latch the slot camera is the walk's (no hand-off between the walk and the latch), so a latch's rows are judged with the camera of the walk that drew them.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../renderer_scene/rows_stage_fx.h"
#include "flash_fx_core.h"   // ReadFn, ReadWord, AsFloat, kYDrop

namespace rdr_scene::live::flashkeep {

inline constexpr uint32_t kEngine = 0x83929A68, kEngineBlock = 256, kEngineScreenW = 300, kEngineScreenH = 304;
inline constexpr uint32_t kCamView = 240, kCamProj = 176;
inline constexpr uint32_t kPhaseWord = 0x836F3940, kMarginsOff = 0x836F3A94, kCharSlots = 0x836F38D0;
inline constexpr uint32_t kCharSlotCount = 9;

// TEST ONLY (tools\scene\flash_camera_unit.cpp): the landed FLASHCAM rule (a 1280 x 720 screen, no character box). The game never sets them.
inline bool g_plant_screen_1280 = false, g_plant_no_char_box = false;

// One latch's inputs of the rule.
struct KeepCam {
  float clip[16] = {};   // +240 . (+176 with row 1 negated), row vectors
  float view[16] = {};   // +240
  float proj_xs = 0.f, proj_ys = 0.f;   // +176 m[0] / m[5] (census: beside our scene projection)
  int32_t w = 0, h = 0;
  uint32_t phase = 0;
  bool margins_off = false;
  uint32_t chars = 0;
  float char_pt[kCharSlotCount][3] = {};
};

inline bool ReadMat16(flashfx::ReadFn read, uint32_t a, float m[16]) {
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t v = 0;
    if (!flashfx::ReadWord(read, a + 4 * i, v)) return false;
    m[i] = flashfx::AsFloat(v);
  }
  return true;
}
inline void Mul(const float a[16], const float b[16], float out[16]) {   // out = a . b (row vectors)
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
inline void Times(const float p[3], const float m[16], float o[4]) {   // (p, 1) . m
  for (int k = 0; k < 4; ++k) o[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
}

// The rule's inputs at this latch. false = the engine, its block or a camera matrix was not readable (the rows are then "not known").
inline bool ReadKeepCam(flashfx::ReadFn read, KeepCam& kc) {
  kc = KeepCam{};
  uint32_t eng = 0, blk = 0, w = 0, h = 0;
  if (!flashfx::ReadWord(read, kEngine, eng) || !eng || !flashfx::ReadWord(read, eng + kEngineBlock, blk) || !blk) return false;
  float proj[16];
  if (!ReadMat16(read, blk + kCamView, kc.view) || !ReadMat16(read, blk + kCamProj, proj)) return false;
  if (!flashfx::ReadWord(read, eng + kEngineScreenW, w) || !flashfx::ReadWord(read, eng + kEngineScreenH, h)) return false;
  kc.w = int32_t(w), kc.h = int32_t(h);
  if (g_plant_screen_1280) kc.w = 1280, kc.h = 720;
  kc.proj_xs = proj[0], kc.proj_ys = proj[5];
  for (int k = 4; k < 8; ++k) proj[k] = -proj[k];
  Mul(kc.view, proj, kc.clip);
  uint32_t m = 0;
  flashfx::ReadWord(read, kPhaseWord, kc.phase);   // unreadable: 0 (the full test)
  kc.margins_off = flashfx::ReadWord(read, kMarginsOff, m) && m != 0;
  for (uint32_t i = 0; i < kCharSlotCount && !g_plant_no_char_box; ++i) {
    uint32_t ch = 0, a = 0, b = 0, c = 0, comp = 0, node = 0;
    if (!flashfx::ReadWord(read, kCharSlots + 4 * i, ch) || !ch) continue;
    if (!flashfx::ReadWord(read, ch + 1208, a) || !a || !flashfx::ReadWord(read, ch + 224, b) || !b || !flashfx::ReadWord(read, ch + 232, c) || !c) continue;   // sub_823CBC50
    if (!flashfx::ReadWord(read, ch + 812, comp) || !comp || !flashfx::ReadWord(read, comp + 4, node) || !node) continue;
    uint32_t v[3];
    if (!flashfx::ReadWord(read, node + 48, v[0]) || !flashfx::ReadWord(read, node + 52, v[1]) || !flashfx::ReadWord(read, node + 56, v[2])) continue;
    for (int k = 0; k < 3; ++k) kc.char_pt[kc.chars][k] = flashfx::AsFloat(v[k]);
    ++kc.chars;
  }
  return true;
}

// The game's answer for an item point (game space, the item's +16..+24): 1 kept, 0 refused.
inline int GameKeeps(const KeepCam& kc, const float p[3]) {
  float vz[4], q[4];
  Times(p, kc.view, vz);
  if (!(vz[2] > 50.f)) return 0;
  if (kc.phase == 4) return 1;
  Times(p, kc.clip, q);
  if (!(std::fabs(q[3]) > 1e-12f)) return 0;
  const float w2 = float(kc.w >> 1), h2 = float(kc.h >> 1);
  const float fx = w2 * (q[0] / q[3] + 1.f), fy = h2 * (q[1] / q[3] + 1.f);
  if (!std::isfinite(fx) || !std::isfinite(fy)) return 0;
  const int32_t px = int32_t(fx), py = int32_t(fy);   // fctiwz: toward zero
  if (px < 64 || py < 64 || px > kc.w - 64 || py > kc.h - 64) return 0;
  const int32_t mx = kc.margins_off ? 0 : 100, my = kc.margins_off ? 0 : 200;
  for (uint32_t i = 0; i < kc.chars; ++i) {
    float r[4];
    Times(kc.char_pt[i], kc.clip, r);
    if (!(std::fabs(r[3]) > 1e-12f)) continue;
    const float cx = w2 * (r[0] / r[3]) + w2, cy = h2 * (r[1] / r[3]) + h2;
    if (!std::isfinite(cx) || !std::isfinite(cy)) continue;
    const int32_t cpx = int32_t(cx), cpy = int32_t(cy);
    if (cpx < 0 || cpx > kc.w) continue;
    if (cpx - mx < px && px < mx + cpx && cpy - my < py && py < my + cpy) return 0;
  }
  return 1;
}

// Stamps one flash row of this latch (pos = the item point with y - 40, flash_fx_core.h kYDrop): pad = the answer (int32: 1 / 0, -1 = not known), w / h = the block's +176 x / y scales.
inline void Stamp(const KeepCam& kc, bool have, StageFxRow& r) {
  const float p[3] = {r.pos[0], r.pos[1] + flashfx::kYDrop, r.pos[2]};
  const int32_t keep = have ? GameKeeps(kc, p) : -1;
  std::memcpy(&r.pad, &keep, 4);
  const float xs = have ? kc.proj_xs : 0.f, ys = have ? kc.proj_ys : 0.f;
  std::memcpy(&r.w, &xs, 4), std::memcpy(&r.h, &ys, 4);
  r.flags |= kFxfKeepCensus;
}

}  // namespace rdr_scene::live::flashkeep
