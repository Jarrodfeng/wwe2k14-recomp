// S102 lane L2 (docs\renderer\SHADOWS_OURS.md, "S102 L2: the ring-plane shadow bake"): the ring mat's rope shadow baked ONCE into a world-plane map, composited per frame. GPU-free core: the
// switch, the knobs, the planted faults, the key, the census, and the two small structs that carry a decision between the binder (renderer_adapter) and the plane hook (renderer_scene).
// No game or SDK includes. Render thread only (the counters are plain; the three atomics are read by tools on other threads).
//
// WHAT IT DOES (R17 section 2.3 / 4, Brief B, re-derived from the captures: the findings are in SHADOWS_OURS.md):
//   The game draws the ring-mat shadow into a 512 mask in the CAMERA's screen space every frame: 24 draws of the ROPE proxy (the ring's ropes seen from 4 eye points) + 8-16 draws of the WRESTLER
//   proxy, downscaled to 256 and blurred twice (the contact-hardening blur), and the ring mat samples that 256 map. The ropes are one static shape: the rope proxy's footprint on the ground plane is the
//   same in every frame (3 lines a side from the 4 eye points, spanning about +-430 world units; the ring itself is +-150); only the camera moves, so the screen-space picture of them swims and is
//   re-rasterised (and re-blurred) every frame.
//   While the switch is on and the frame's chain is a clean four-pass chain, this lane
//     1. keeps a BAKE: the rope proxy's mask, drawn once into a WORLD-plane map (size knob, default 1024, centre = the plane's position, half extent 450 units) by the plane hook's OWN mask program with an
//        ortho matrix, softened once by two separable passes. Key = the rope draws' constants (fade, plane position, eye, bone palette) within a tolerance of the baked ones, plus the rope object, the
//        plane's height and the size: a hit draws nothing; a miss re-bakes (counted).
//     2. leaves the rope draws out of the game's mask pass (the mask now holds the WRESTLER proxy only; the game's downscale and blurs and the copy into the mat's map run as the stream has them);
//     3. composites per frame: ONE pass into a bigger map (size / 2) that holds max(the game's blurred wrestler map, the bake reprojected through this frame's camera fit), and the ring mat's reads of the
//        game's map are pointed at the composite. The composite is in the SAME screen-fit space as the game's map, so the ring mat's shader is untouched.
// Offline proof: tools\shadow\plane_bake_check.py (fview headless). Switch: wwe2k14_rdr_plane_bake (PerFrame, default OFF; rdr_switches.h); fview --plane-bake[=SIZE].
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdr_adapter::plane_bake {

inline std::atomic<bool> g_fview{false};     // fview's fallback for the switch (the live table is not running there): --plane-bake
inline std::atomic<uint32_t> g_size{1024};   // the bake's edge in texels (256 .. 2048): live wwe2k14_rdr_plane_bake_size, fview --plane-bake=SIZE; the composite is half of it
inline float g_extent = 450.0f;   // half the bake's side in world units (fview --plane-bake-extent). Measured on Check B (the bake at 2048 around the plane position, extent 1200): the rope proxy's shadow is 3 lines a side, from the 4 eye points,
                                   // spanning about +-430 units on x and z (the ring itself is +-150): 450 holds all of it, with the census counting a lit texel on the border (edge texels) as the guard
inline constexpr float kTolerance = 0.25f;
inline float g_rot_tol = 1.0e-2f;   // the tolerance of every key float that is not a translation (fview --plane-bake-rot-tol). Check B, rebakes in 341 frames / line centre offset vs the game: 1e-3: 251 / 0.78 px; 3e-3: 103 / 0.88; 1e-2: 38 / 0.83; 3e-2: 15 / 0.95 (the rope sways a little all the time; a stale bake costs about a tenth of a pixel)   // largest |difference| (world units) of any rope constant against the baked one that still counts as the same shape (a quarter unit = under a texel at 1024)
inline constexpr uint32_t kBankRows = 256;   // a constant bank: 256 vec4
inline constexpr uint32_t kPaletteFirst = 76, kPaletteRows = 63;   // the mask's bone palette c76.. (63 rows)
inline constexpr uint32_t kEyeRow = 72;      // EyePos c72
inline constexpr uint32_t kFadeRows = 9;     // c0..c8: the fade array, the plane position (c7), the plane normal (c8)

// Planted faults (tools only; a player build never sets them; fview --plane-bake-fault NAME). Each must fail its named check in tools\shadow\plane_bake_check.py.
enum class Fault : uint8_t {
  None = 0,
  WrongArena,     // the bake is made around a centre 60 units off (another arena's plane position): the rope lines sit where the game's do not
  NoWrestler,     // the composite leaves the game's wrestler map out: the mat loses the wrestler's contact shadow (past bug: lane 191's "is the ring-mat shadow missing")
  BakeLate,       // the composite is issued BEFORE the bake pass of a frame: the first frame (and every re-bake frame) reads the bake that was there, or nothing
  HalfTexel,      // the bake is read half a texel off (the half-pixel quirk the 360 projection carried): the lines slide
  CompositeMin,   // min instead of max in the composite
  StaleKey,       // the key never invalidates: a rope shape that moved keeps the old bake
  Scale5,         // the composite reads the bake at 95 % of its scale: every rope line is off by up to 5 % of its distance from the centre (the line-geometry check must fail it)
  CentreNow,      // the composite reads the bake around a centre 30 units off the one it was made around (what a centre taken from another frame than the bake's does when c7 moves 30 between them) (F2)
  CompositeTickCamera,   // the composite's camera rows are not declared as tick-blended ranges (the lint / pump check must fail it)
  NoBake,         // (a measuring arm, not a plant) the composite ignores the bake: wrestler map only
  RopesKept,      // the rope draws stay in the game's mask pass although the bake took them: the ropes are drawn twice
  Count
};
inline Fault g_fault = Fault::None;
inline float g_soft = 1.6f;   // the soften's gaussian sigma in WORLD units (the same look at every bake size); calibrated against the game's rope bars: 1.5-1.6 units gives the same line width (ratio 0.95), centre within 1 px (tools/shadow/plane_bake_lines.py); fview --plane-bake-soft
inline float g_gain = 1.0f;   // the bake's strength in the composite: calibrated so the bake's integrated darkening is about the game's (energy ratio 1.0-1.1 on Check B, tools/shadow/plane_bake_check.py); fview --plane-bake-gain
inline const char* FaultName(Fault f) {
  static const char* const k[] = {"none", "wrong_arena", "no_wrestler", "bake_late", "half_texel", "composite_min", "stale_key", "scale5", "centre_now", "composite_tick_camera", "no_bake", "ropes_kept"};
  return uint8_t(f) < uint8_t(Fault::Count) ? k[uint8_t(f)] : "?";
}
inline bool ParseFault(const std::string& text, std::string& error) {
  for (uint8_t i = 0; i < uint8_t(Fault::Count); ++i)
    if (text == FaultName(Fault(i))) {
      g_fault = Fault(i);
      return true;
    }
  error = "unknown plane-bake fault '" + text + "' (wrong_arena, no_wrestler, bake_late, half_texel, composite_min, stale_key, scale5, centre_now, composite_tick_camera, ropes_kept)";
  return false;
}

// TEST ONLY (fview --plane-bake-wobble LO,HI,AMP): the rope proxy's bone palette is translated by up to AMP world units along x on the replayed frames [LO, HI) (the bind count from 0) (a ramp over the first 10 frames), in the
// frame's own banks, before any planner or hook reads them: every arm (switch on, off, planted) sees the same moved ropes. The captured matches never move the ropes (the whole of Check B drifts 0.23
// units), so this is the only way to drive the key's invalidation offline.
inline uint64_t g_wobble_lo = 0, g_wobble_hi = 0;
inline float g_wobble_c7 = 0.0f;       // TEST ONLY (fview --plane-bake-wobble-c7 AMP): the plane position c7.x of every plane draw moves by up to AMP in the same window
inline float g_wobble_c1 = 0.0f;       // TEST ONLY (fview --plane-bake-wobble-c1 AMP): the fade array's c1.x of every plane draw moves by AMP in the same window (a non-translation constant: must re-bake)
inline float g_wobble_amp = 0.0f;

// What a draw is made for, set by the binder around one BindDraw and read by the plane hook (which answers with its own program and constants for the variant).
enum Variant : uint8_t {
  kVariantNone = 0,   // an ordinary chain draw
  kVariantBake,       // a rope proxy draw into the bake's raw map: the mask program, the ortho matrix
  kVariantSoftenX,    // one fullscreen pass: the raw / soft map softened along x
  kVariantSoftenZ,    // ... along z
  kVariantComposite,  // the per-frame composite
};
struct Call {
  Variant variant = kVariantNone;
  uint32_t slot0 = 0, slot1 = 0;   // renderer image ids the hook names in the draw's texture slots 0 and 1 (slot 1 is the binder's extra slot)
  float m[16] = {};                // the frame's screen fit M = PlaneInv * CameraViewProj (row major), taken by the binder from a plane draw's own rows at plan time
  float plane_y = 0;
  uint32_t target_w = 0;           // the target's edge (texels): the soften taps' step
  uint32_t bake_size = 0;          // the bake's edge
  float centre[2] = {0, 0};        // the bake's centre on the plane (x, z), from the plane position
  float extent = 160.0f;
};
inline Call g_call;

// What the hook tells the binder (a frame's facts, hook -> binder; rewritten by the hook on every verified mask draw).
struct Shared {
  uint32_t rope_object = 0;        // the engine object whose mask draws use 3+ distinct eye points in one frame (the rope proxy: 4 eyes; a wrestler's proxy has one); 0 = not learned yet
  uint32_t rope_object_frames = 0; // consecutive frames it was seen
  bool proj_valid = false;         // this frame's screen fit and plane height were taken from a verified mask draw (cleared by the binder at the start of a frame)
  float m[16] = {};                // M = PlaneInv * CameraViewProj of the frame (row major, as the shader multiplies: M * (x, y, z, 1)); the plane's screen-fit projection
  float plane_y = 0, plane_x = 0, plane_z = 0;   // c7: the plane's position
  uint32_t proj_frame_token = 0;   // bumped on every verified mask draw (a census of how many)
  bool bake_program_ok = false;    // the hook made the bake's fullscreen program (a driver that refuses it leaves the lever off)
};
inline Shared g_shared;

// ---- the key -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// A rope draw's constants the shape depends on: c0..c8 (fade, plane position, normal), c72 (eye) and the palette c76..c138. The screen fit (c9..c16) is the camera's and is NOT part of the key.
// `bank` = the 256 x 4 dwords of the draw's vertex constant bank. Written into `out` (kBankRows * 4 floats are NOT copied: only the key rows, packed).
inline constexpr uint32_t kKeyRows = kFadeRows + 1 + kPaletteRows;   // 9 + 1 + 63
inline constexpr uint32_t kKeyFloats = kKeyRows * 4;
inline void PackKey(const uint32_t* bank, float* out) {
  std::memcpy(out, bank, kFadeRows * 16);
  std::memcpy(out + kFadeRows * 4, bank + kEyeRow * 4, 16);
  std::memcpy(out + (kFadeRows + 1) * 4, bank + kPaletteFirst * 4, kPaletteRows * 16);
}
// The planted fault StaleKey: no difference is ever too big. Returns the largest |a - b| over the key floats; NaN counts as infinite.
// The tolerance of float i of the key: a quarter world unit for the TRANSLATIONS (the palette rows' w, c7 xyz, EyePos xyz: a quarter unit is under a texel at 1024), 1e-3 for everything else (rotations, the fade
// array, the plane normal: a rotation error of 1e-3 moves a rope end 0.15 units on a 150 unit ring).
inline float KeyTolerance(uint32_t i) {
  const uint32_t row = i / 4, lane = i % 4;
  if (row < kFadeRows) return (row == 7 && lane < 3) ? kTolerance : g_rot_tol;
  if (row == kFadeRows) return lane < 3 ? kTolerance : g_rot_tol;   // EyePos
  return lane == 3 ? kTolerance : g_rot_tol;                       // a palette row: w is the translation
}
// The largest |a - b| / tolerance over the key floats: under 1 = the same shape. NaN counts as infinite.
inline float KeyRatio(const float* a, const float* b) {
  float worst = 0;
  for (uint32_t i = 0; i < kKeyFloats; ++i) {
    const float d = std::fabs(a[i] - b[i]) / KeyTolerance(i);
    if (!(d <= worst)) worst = std::isnan(d) ? 1.0e30f : d;
  }
  return worst;
}

// ---- the census ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Counters {
  uint64_t frames = 0;                // frames with a plane chain in the stream
  uint64_t taken = 0;                 // frames the lever took (the rope draws left out of the mask pass, the composite made)
  uint64_t hits = 0;                  // ... of which the bake was kept (nothing drawn for the ropes)
  uint64_t rebakes = 0;               // ... of which the bake was made again
  uint64_t declined = 0;
  uint64_t d_switch = 0, d_blank = 0, d_chain = 0, d_extra = 0, d_no_rope = 0, d_overlap = 0, d_hook = 0, d_images = 0, d_other = 0, d_skip = 0;   // by reason
  uint64_t rope_draws_left_out = 0;   // rope proxy draws not recorded in the mask pass (24 a taken frame)
  uint64_t bake_draws = 0;            // rope draws made into the bake (24 a re-bake)
  uint64_t passes_added = 0;          // passes the lever adds (3 on a re-bake: raw + 2 softens; 1 on a hit: the composite)
  uint64_t edge_texels = 0;           // (not measured live: a readback; plane_bake_check.py reads the raw bake's border from fview's dump and must find 0)
  uint64_t half_skip_kept = 0;        // half-rate skipped frames whose mat read the last composite (consistency kept)
  uint64_t reads_pointed = 0;         // texture reads of the game's map pointed at the composite
  uint64_t composite_no_proj = 0;     // (unused since the binder takes M from a plane draw's own rows)
  uint64_t early_reads = 0;           // reads of the game's map by a draw BEFORE the composite of a taken frame (must stay 0: the mat would read the stale map)
  uint64_t unsteady = 0;              // frames declined because the rope shape kept changing (a bounce): the game's chain draws the ropes until the key is steady
};
inline std::string CensusLine(const Counters& c, uint32_t size) {
  char line[900];
  std::snprintf(line, sizeof line,
                "[rdr][plane_bake] frames %llu, taken %llu, hits %llu, rebakes %llu, declined %llu (switch %llu, blank %llu, chain %llu, extra %llu, no rope %llu, overlap %llu, hook %llu, images %llu, skip %llu, unsteady %llu, other %llu); "
                "rope draws left out %llu, bake draws %llu, passes added %llu, edge texels %llu, reads pointed %llu, half-skip kept %llu, early reads %llu; bake %u",
                (unsigned long long)c.frames, (unsigned long long)c.taken, (unsigned long long)c.hits, (unsigned long long)c.rebakes, (unsigned long long)c.declined, (unsigned long long)c.d_switch,
                (unsigned long long)c.d_blank, (unsigned long long)c.d_chain, (unsigned long long)c.d_extra, (unsigned long long)c.d_no_rope, (unsigned long long)c.d_overlap, (unsigned long long)c.d_hook,
                (unsigned long long)c.d_images, (unsigned long long)c.d_skip, (unsigned long long)c.unsteady, (unsigned long long)c.d_other, (unsigned long long)c.rope_draws_left_out, (unsigned long long)c.bake_draws,
                (unsigned long long)c.passes_added, (unsigned long long)c.edge_texels, (unsigned long long)c.reads_pointed, (unsigned long long)c.half_skip_kept, (unsigned long long)c.early_reads, size);
  return line;
}

}  // namespace rdr_adapter::plane_bake
