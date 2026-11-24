// The post settings object S as the scene record carries it (S103 lane A; docs\renderer\INPUTS_BOARD.md rows P1-P9, docs\systems\post_effects.md "The settings object S").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header-only, no game, SDK or renderer includes: the latch reader (inputs_contrib.h), the offline unit test and the post pass all include this file.
//
// What it is: S = the guest word at 0x8392DD88 points at one block of post parameters. Every post effect reads its flag and numbers from S by byte offset (post_effects.md, listings read by lane PR,
// Session 101: [C] = read in a listing there). The post node's walk (sub_82C1E840) reads them inside the walk; under landing 1 that walk does not run, so the record carries S itself
// (PostParamsRow::s, words = S + 4 * i) and the post pass derives the constants below from it. Only the derivations lane PR settled ([C]) are here; anything else is read as a raw word.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../renderer_scene/rows_inputs.h"

namespace rdr_scene::post_settings {

inline constexpr uint32_t kGuestPointer = 0x8392DD88;   // [C post_effects.md] the guest word that holds S's address
inline constexpr uint32_t kBytes = 4 * kPostSettingsWords;

// Byte offsets into S (post_effects.md "The settings object S" table; each [C] unless noted).
enum Off : uint32_t {
  kOn = 0,          // non-zero: the post module runs (both the walk and the update test it)
  kDof = 4,         // DoF on
  kDepth8 = 8,      // depth-buffer values the DoF ramps use (c20.z = S+8)
  kDepth12 = 12,
  kDepth16 = 16,
  kFarCap = 20,     // c20.w
  kDepth24 = 24,
  kDepth28 = 28,
  kFloor = 32,      // c21.z
  kBlurChain = 168, // 1 = depth-aware
  kStrength = 172,  // c21.w of the downsample pass
  kBloom = 176,     // BLOOM on
  kTonemap = 180,   // TONEMAP on
  kNight = 184,     // the third HDR flag (FX_NIGHT [L])
  kBloomC31 = 188,  // c31
  kBloomC32 = 192,  // c32
  kStarAngle = 200, // star glare rotation = render view +640 + +644 (set every frame by sub_827F2488)
  kMonotone = 208,  // monotone on; colour at +212..+220 (c0 = +212, c1 = +216, c2 = +220: sub_82C2A6E0 +104 / +118 / +12C [C], FXP.md adversary C)
  kAntialias = 280,   // S103 PST: antialiasing on (effect 0); the threshold (g_AntiThreshold, c1) at +284 [C COV9 2e]
  kAntiThreshold = 284,
  kHala300 = 300,     // effect 5's switch (sub_82C2A540, not halation); ctor 0, no writer [C HRB.md 4]
  kHala304 = 304,     // halation master (ctor 1, no other writer) [C HRB.md 2]
  kHala308 = 308,     // halation trigger: written only via the 'HALA' match component (sub_82699DF0 / sub_82699D70) [C HRB.md 3]
  kRadial = 524,      // radial blur draw count (ctor 3), read only inside the halation gate [C HRB.md 2]
  kAfterOn = 224,    // S103 FXF: yAfterImage (post effect 4) on; the walk needs S+0 != 0 as well [C, FXP.md adversary A]
  kAfterAlpha = 228,  // f32: the ghost's alpha (floor(x * 255) / 255)
  kAfterX = 232,      // f32: the sprite CENTRE, in pixels of the game's display (kDisplayW x kDisplayH)
  kAfterY = 236,
  kAfterScale = 240,  // f32: the sprite's scale about its centre
  kAfterR = 256,      // u32 0..255 tint (S+268 is never read)
  kAfterG = 260,
  kAfterB = 264,
  kAfterBlend = 272,  // u32: 0 = SRC_ALPHA / INV_SRC_ALPHA, 1 = SRC_ALPHA / ONE (additive); S+276 (the per-frame resolve) is the constant 1
  kDefocus = 560,   // defocus on; strength at +564
  kDefocusRate = 564,    // S103 FXH: g_f1DefocusRate (cue 12: 0.667, cue 13: 0.5) [C COV9 2b]
  kDefocusColour = 576,  // S103 FXH: the hue colour (R, G, B, A) at +576..+588; sub_82C292D0 turns it into (Y, Cb, Cr, A) [C COV9 2b]
  kEmboss = 592,    // emboss on; strength at +596
  kEmbossStep = 596,     // S103 FXH: the sample step in DISPLAY pixels (cue 14: 0.5, 15: 1, 16: 2) [C COV9 2c]
  kRetro = 604,     // retro film on; three image handles in +608..+632
  kRetroImages = 608,    // S103 FXH: guest texture handles (strips +608 / +616, grain +624): not pictures in the record, [U] source (post_cue_fx.h)
  kRetroRange = 640,     // S103 FXH: g_Range (gain, offset, min, max) at +640..+652 (cue 12: .7 .3 .3 .8, cue 13: .7 .3 .3 .9) [C COV9 2d]
  kFog = 656,       // FOG on; haze values +664..+692 (c27 = +676, +680, +684, 1.0)
  kFogC27 = 676,
};

// The game's display size, the unit of S+232 / +236 (the sprite centre): [[0x83929A68]+300] x +304, 1280 x 720 expected [U: not read live; FXP.md adversary E]. The centre is a fraction of it.
inline constexpr float kDisplayW = 1280.f, kDisplayH = 720.f;

// S103 FXH (COV9 2b): sub_82C292D0, the defocus hue colour: (R, G, B, A) -> (Y, Cb, Cr, A), the BT.601 weights of the game's own yDefocus / yRetroFilm shaders.
inline void YCbCrFrom(const float rgba[4], float out[4]) {
  out[0] = 0.29891f * rgba[0] + 0.58661f * rgba[1] + 0.11448f * rgba[2];
  out[1] = -0.16874f * rgba[0] - 0.33126f * rgba[1] + 0.5f * rgba[2];
  out[2] = 0.5f * rgba[0] - 0.41869f * rgba[1] - 0.08131f * rgba[2];
  out[3] = rgba[3];
}

inline uint32_t Word(const PostParamsRow& r, uint32_t byte_off) { return byte_off / 4 < kPostSettingsWords ? r.s[byte_off / 4] : 0u; }
inline float Float(const PostParamsRow& r, uint32_t byte_off) {
  const uint32_t w = Word(r, byte_off);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// The flags word a row carries, computed from its words (the writer and the offline tests use the same function; a reader may recompute it to check the row).
inline uint32_t FlagsOf(const PostParamsRow& r, bool read_ok) {
  uint32_t f = read_ok ? uint32_t(kPpRead) : 0u;
  if (Word(r, kOn)) f |= kPpOn;
  if (Word(r, kDof)) f |= kPpDof;
  if (Word(r, kBloom)) f |= kPpBloom;       // the walk tests these words for non-zero (post_effects.md 6: S+176 or S+184 or S+180)
  if (Word(r, kTonemap)) f |= kPpTonemap;
  if (Word(r, kNight)) f |= kPpNight;
  if (Word(r, kFog)) f |= kPpFog;
  if (Word(r, kMonotone)) f |= kPpMonotone;
  if (Word(r, kDefocus)) f |= kPpDefocus;
  if (Word(r, kEmboss)) f |= kPpEmboss;
  if (Word(r, kRetro)) f |= kPpRetro;
  if (Word(r, kAfterOn)) f |= kPpAfterImage;
  if (Word(r, kAntialias)) f |= kPpAntialias;
  if (Word(r, kHala300)) f |= kPpHala300;
  // S103 HCN (HRB.md 1 [C], sub_82C29818): halation runs iff S+0 && S+304 && S+308; the radial blur needs S+524 > 0 inside it. The raw words are ctor defaults (304 = 1, 524 = 3), never the signal.
  if (Word(r, kOn) && Word(r, kHala304) && Word(r, kHala308)) {
    f |= kPpHalation;
    if (static_cast<int32_t>(Word(r, kRadial)) > 0) f |= kPpRadial;
  }
  return f;
}

// The combine's DoF constants as sub_82C1C1A0 writes them (0x82C1C1A0-0x82C1C260, [C] post_effects.md 1):
//   c20 = (1 / (S24 - S12), 1 / (S16 - S28), S8, S20),  c21 = (S24, S28, S32, 0).
// A zero denominator gives +-inf exactly as the game's divide does (c20.x = inf is how "near blur off" shows: S24 = S12 = 0); the caller clamps as the shader does.
struct Combine {
  float c20[4];
  float c21[4];
  float c27[4];   // fog / haze: (S676, S680, S684, 1.0)  [C]
  float c31, c32; // bloom: S188, S192 (the arena table stores them x 0.01 already)  [C]
};
inline Combine CombineFrom(const PostParamsRow& r) {
  Combine c{};
  c.c20[0] = 1.f / (Float(r, kDepth24) - Float(r, kDepth12));
  c.c20[1] = 1.f / (Float(r, kDepth16) - Float(r, kDepth28));
  c.c20[2] = Float(r, kDepth8);
  c.c20[3] = Float(r, kFarCap);
  c.c21[0] = Float(r, kDepth24);
  c.c21[1] = Float(r, kDepth28);
  c.c21[2] = Float(r, kFloor);
  c.c21[3] = 0.f;
  c.c27[0] = Float(r, kFogC27);
  c.c27[1] = Float(r, kFogC27 + 4);
  c.c27[2] = Float(r, kFogC27 + 8);
  c.c27[3] = 1.f;
  c.c31 = Float(r, kBloomC31);
  c.c32 = Float(r, kBloomC32);
  return c;
}

}  // namespace rdr_scene::post_settings
