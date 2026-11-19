// Session 103, lane FXH: the three highlight-reel cue post effects of the stream-free post pass (docs\research\s103\COV9_story.md 2b / 2c / 2d; docs\systems\post_effects.md "Cue effects").
// Header-only, no renderer, no game: the retro film's clock (sub_82C288B0) and our own noise pictures, used by post_pass.cpp and by the offline unit (tools\scene\post_rig.cpp FxhTest).
//
// What the game does (all [C], q decomp sub_82C288B0 / sub_82C28A70 / sub_82C28F68 / sub_82C29478 / sub_82C292D0):
//   defocus + hue (S+560): out = hue-shift(mix(frame, DoF blur, S+564), (Y, Cb, Cr, A) of the colour S+576..588);   emboss (S+592): a relief of the frame's RED channel with a step of S+596 display pixels;
//   retro film (S+604): shake + flicker + grain on the luma, three pictures (two one-column noise strips read at (0, t) and one grain texture) whose handles are S+608 / +616 / +624.
// The three retro pictures are guest textures the cue record names (+68 list); the record carries the handles, never the pixels, and which pac entries they are is [U] (COV9 section 6). So the pictures here are OUR OWN,
// made from a fixed seed: uniform noise strips (the shader uses the .x channel: jitter = (2 n - 1)^3 x 4 / H pixels, flicker = n) and a grain texture whose ALPHA is read (the luma is multiplied by it). Their statistics
// are a placeholder for the game's [U]: the look (a flickering, shaking, grainy luma) is the game's, the exact grain pattern is not.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace rdr_scene::cue_fx {

inline constexpr uint32_t kStripH = 256;          // the noise strips: 1 x 256, read at (0, t) with a POINT sampler (the game's slots 1 / 2)
inline constexpr uint32_t kGrainW = 256, kGrainH = 256;   // the grain texture: wrap + linear (the game's slot 3), uv scaled by 1/3 and 0.1333 in the shader
inline constexpr uint32_t kSeedStrip1 = 0x51A1u, kSeedStrip2 = 0xB2C3u, kSeedGrain = 0x7E57u;   // the fixed seeds of our three pictures (slots 1 / 2 / 3)
inline constexpr uint64_t kRetroGap = 30;        // guest frames one retro step may compose; a frame number that goes BACK by more than this starts the clock again (a new scene)
inline constexpr float kRetroRateDefault = 30.f;   // the game's frame rate [0x8392DC58] when the latch could not read it (the entrance / cue scenes' cap)
inline bool RatePlausible(float r) { return std::isfinite(r) && r >= 10.f && r <= 240.f; }
// CLOCK DIFFERENCE: the game ticks this clock once per game update at dt = 1 / its frame rate (30 or 60); ours ticks once per GUEST FRAME the record carries, with the same rate the latch read, so the grain / flicker run at the game's speed whatever our display or tween rate is.

// sub_82C288B0, the retro film's update (once per frame, dt = 1 / frame rate): obj+44 = fmod(obj+44 + dt, 1); obj+56 += dt; whenever obj+56 >= 0.05 (0x8362A06C): obj+56 -= 0.05,
// obj+48 = fmod(obj+48 + dt, 1), obj+52 = fmod(obj+52 + 35.2517 dt, 1) (0x8206D76C). The fields are f32 in the game; fmod is the f64 one (0x82C7A628) with divisor 1.0.
struct RetroClock {
  float t44 = 0.f, t48 = 0.f, t52 = 0.f, acc = 0.f;
  uint64_t last = 0;
  bool armed = false;
  uint64_t steps = 0;   // distinct frames stepped
};

inline void RetroTick(RetroClock& c, float rate) {
  const float dt = 1.f / rate;
  c.t44 = float(std::fmod(double(c.t44 + dt), 1.0));
  c.acc = c.acc + dt;
  if (c.acc >= 0.05f) {
    c.acc = c.acc - 0.05f;
    c.t48 = float(std::fmod(double(c.t48 + dt), 1.0));
    c.t52 = float(std::fmod(double(c.t52 + 35.2517f / rate), 1.0));
  }
}

// One call per Record that draws the retro film: IDEMPOTENT per guest frame (the 120 fps tween records the same guest frame twice, the dev mirror draws it again: a repeat reads the same clock, as the FXF history and
// the PST adapted luminance do). A frame more than kRetroGap behind the last (or the first use) restarts the clock at 0. A forward gap composes the frames that were not drawn (k <= kRetroGap): the game ticked them.
// `every_record` is the RIG-ONLY planted twin (a tick on every Record, however often a guest frame is recorded).
inline void RetroAdvance(RetroClock& c, uint64_t guest_frame, float rate, bool every_record = false) {
  const bool fresh = !c.armed || guest_frame + kRetroGap < c.last;
  if (fresh) {
    c = RetroClock();
    c.armed = true, c.last = guest_frame;
    return;
  }
  if (!every_record && guest_frame <= c.last) return;   // the same guest frame again (or a step back inside the gap): no new tick
  // render-clock: ctl - counts the guest frames not drawn (capped) so the game's own per-frame tick runs that many times; the tick size is the game's 1 / rate, not a wall clock
  const uint64_t k = every_record ? 1 : (guest_frame - c.last > kRetroGap ? kRetroGap : guest_frame - c.last);
  for (uint64_t i = 0; i < k; ++i) RetroTick(c, rate);
  if (!every_record) c.last = guest_frame;
  ++c.steps;
}

// Our own noise pictures (a fixed seed: the same bytes on every machine). RGBA8 texels, all four channels the same value; the strips are uniform in [0, 1], the grain's alpha is 0.78 + 0.22 u (mean 0.89).
inline uint32_t Hash32(uint32_t x) {
  x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
  return x;
}
inline std::vector<uint8_t> MakeStrip(uint32_t seed) {
  std::vector<uint8_t> px(size_t(kStripH) * 4);
  for (uint32_t i = 0; i < kStripH; ++i) {
    const uint8_t v = uint8_t(Hash32(i * 2654435761u + seed) >> 24);
    for (int c = 0; c < 4; ++c) px[size_t(i) * 4 + c] = v;
  }
  return px;
}
inline std::vector<uint8_t> MakeGrain(uint32_t seed) {
  std::vector<uint8_t> px(size_t(kGrainW) * kGrainH * 4);
  for (uint32_t i = 0; i < kGrainW * kGrainH; ++i) {
    const float u = float(Hash32(i * 2654435761u + seed) >> 8) / float(1u << 24);
    const uint8_t v = uint8_t(std::lround((0.78f + 0.22f * u) * 255.f));
    for (int c = 0; c < 4; ++c) px[size_t(i) * 4 + c] = v;
  }
  return px;
}

}  // namespace rdr_scene::cue_fx
