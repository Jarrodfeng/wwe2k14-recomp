// S102 lane R18-B2 (docs\renderer\COMBINE_LITE.md): the owned combine does less per pixel. No Vulkan, no game here: the switch, the planted faults, the counters and the one function that
// names the variant, shared by post_pass.cpp (the pick), live.cpp (the per-frame read), fview / rplay (the flags) and tools\r18.
//
// wwe2k14_rdr_combine_lite (PerFrame, default 0), read where the combine variant is picked, every frame:
//   1  the EXACT early-outs: (a) when neither the bloom blur nor the glare is recorded (their images are black) the combine permutation WITHOUT bloom is picked and the two clears are not made;
//      (b) in yGlareEtc.frag FX_LITE a DoF blur whose weight is 0 and a bloom / star image whose scale is 0 are not FETCHED (the arithmetic after the fetch is the old one, with a texel of 0).
//      Byte-equal on NVIDIA (Bryan, the match, Brock, wm30), "exact up to floating-point contraction": the SPIR-V carries no NoContraction, so another driver may fuse the changed code differently
//      by one unit in the last place. A far-blur (slot 6) texel that is NaN / Inf made the old path NaN where the weight was 0 and gives the clean colour here: a fix, not a regression.
//      The fog mix is NOT changed (ALU only, no fetch). A removal of fetches and passes, not a cache.
//   2  + the flat-pixel exit (FX_LITE_FXAA): FXAA 3.11's own early exit (range of the five taps under max(1/32, max / 8)) returns the centre tap; the four smoothing taps are not made.
//      NOT byte-equal: a LOOK lever, judged by a sheet and a clip, never decided by this lane.
// Tools only (g_combine_lite_plant, fview / rplay --combine-lite-plant N): 1 the DoF blur is never fetched (lite_dof_always_skip), 2 the bloom is never fetched (lite_bloom_skip_live),
// 3 the flat threshold is 10x (lite_flat_wide), 4 the pixels the flat exit takes are drawn magenta (the share instrument, read from the picture), 5 the variant is never picked
// (lite_not_applied: the census must read 0 draws). Plants 1 and 2 are level-1 faults (their twins have no flat exit) and are used at level 1 and 2 alike; 3 and 4 are level-2 only.
// A planted twin exists for a few permutations (4 for every one); for any other the plain lite variant is used.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdr {

inline std::atomic<int> g_combine_lite{0};         // 0 off, 1 exact early-outs, 2 + the flat exit
inline std::atomic<int> g_combine_lite_plant{0};   // tools only, see above
// Census, whole run (relaxed counters, bumped once per combine DRAW, never per pixel): combine draws recorded, draws that took each level, and the frames seen.
inline std::atomic<uint64_t> g_combine_lite_draws{0}, g_combine_lite_level1{0}, g_combine_lite_level2{0};
// What the combine was given (review: "the bokeh skip fires only if DOFParam2.z <= 0: unmeasured"): draws that used the permutation without bloom, draws with DOFParam2.z <= 0, BloomScale == 0,
// StarScale == 0, and the last values seen (float bits). Relaxed stores / adds in CombineLiteNote, once per combine draw; printed with the census.
inline std::atomic<uint64_t> g_combine_lite_nobloom{0}, g_combine_lite_z_le0{0}, g_combine_lite_bloom0{0}, g_combine_lite_star0{0};
inline std::atomic<uint32_t> g_combine_lite_last_z{0}, g_combine_lite_last_bloom{0}, g_combine_lite_last_star{0};

inline uint32_t CombineLiteBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float CombineLiteFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline void CombineLiteNote(bool dropped_bloom, float dof_z, float bloom_scale, float star_scale) {
  if (dropped_bloom) g_combine_lite_nobloom.fetch_add(1, std::memory_order_relaxed);
  if (dof_z <= 0.f) g_combine_lite_z_le0.fetch_add(1, std::memory_order_relaxed);
  if (bloom_scale == 0.f) g_combine_lite_bloom0.fetch_add(1, std::memory_order_relaxed);
  if (star_scale == 0.f) g_combine_lite_star0.fetch_add(1, std::memory_order_relaxed);
  g_combine_lite_last_z.store(CombineLiteBits(dof_z), std::memory_order_relaxed);
  g_combine_lite_last_bloom.store(CombineLiteBits(bloom_scale), std::memory_order_relaxed);
  g_combine_lite_last_star.store(CombineLiteBits(star_scale), std::memory_order_relaxed);
}

inline int ClampCombineLite(int v) { return v < 0 ? 0 : (v > 2 ? 2 : v); }

// The suffix the combine variant takes at the current switch ("" = the game's own permutation), and bumps the census. `exists` says whether a registry variant of that name is there.
template <class Exists>
inline std::string CombineLiteName(const std::string& base_name /* "yGlareEtc_dof_fog_bloom_fragment" */, Exists exists) {
  g_combine_lite_draws.fetch_add(1, std::memory_order_relaxed);
  const int level = ClampCombineLite(g_combine_lite.load(std::memory_order_relaxed));
  const int plant = g_combine_lite_plant.load(std::memory_order_relaxed);
  if (level == 0 || plant == 5) return base_name;
  const std::string stem = base_name.substr(0, base_name.size() - std::string("_fragment").size());
  if (level >= 2 && plant >= 1 && plant <= 4) {   // a planted twin only where the permutation has one (level 2 carries the flat exit; plants 1 and 2 are level-1 faults too)
    static const char* const kSuffix[5] = {"", "_litep1", "_litep2", "_litep3", "_litem"};
    const std::string planted = stem + kSuffix[plant] + "_fragment";
    if (exists(planted.c_str())) { g_combine_lite_level2.fetch_add(1, std::memory_order_relaxed); return planted; }
  }
  if (level == 1 && plant >= 1 && plant <= 2) {
    const std::string planted = stem + (plant == 1 ? "_litep1" : "_litep2") + "_fragment";
    if (exists(planted.c_str())) { g_combine_lite_level1.fetch_add(1, std::memory_order_relaxed); return planted; }
  }
  if (level >= 2) {
    const std::string lite = stem + "_lite2_fragment";
    if (exists(lite.c_str())) { g_combine_lite_level2.fetch_add(1, std::memory_order_relaxed); return lite; }
  }
  const std::string lite = stem + "_lite1_fragment";   // level 1, or level 2 on a permutation with nothing to skip (the base one has no _lite1: it stays the game's own)
  if (exists(lite.c_str())) { g_combine_lite_level1.fetch_add(1, std::memory_order_relaxed); return lite; }
  return base_name;
}

// The census line, printed every 600 frames while the switch is on (live.cpp) and once at the end of a tool run.
inline void CombineLiteCensusLine(uint64_t frames, char* out, size_t cap) {
  std::snprintf(out, cap,
                "[rdr][combine_lite] frames %llu, combine draws %llu, with lite %llu (level 1 %llu, level 2 %llu), no-bloom permutation %llu, DOFParam2.z<=0 %llu, BloomScale==0 %llu, StarScale==0 %llu, last z %.4f bloom %.4f star %.4f",
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(g_combine_lite_draws.load()), static_cast<unsigned long long>(g_combine_lite_level1.load() + g_combine_lite_level2.load()),
                static_cast<unsigned long long>(g_combine_lite_level1.load()), static_cast<unsigned long long>(g_combine_lite_level2.load()), static_cast<unsigned long long>(g_combine_lite_nobloom.load()),
                static_cast<unsigned long long>(g_combine_lite_z_le0.load()), static_cast<unsigned long long>(g_combine_lite_bloom0.load()), static_cast<unsigned long long>(g_combine_lite_star0.load()),
                CombineLiteFloat(g_combine_lite_last_z.load()), CombineLiteFloat(g_combine_lite_last_bloom.load()), CombineLiteFloat(g_combine_lite_last_star.load()));
}

}  // namespace rdr
