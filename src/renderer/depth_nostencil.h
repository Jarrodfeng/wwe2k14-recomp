// S102 lane R24-C (docs\renderer\DEPTH_NOSTENCIL.md): stencil liveness. No Vulkan, no game, no SDK here: the switch, the counters, the planted faults, the stencil census the renderer fills and
// the pure rule tools\r24c\ share.
//
// The format route (depth images made D32_SFLOAT, promoted to D32S8 on a stencil use) was built and DROPPED (review, 2026-11-15): vkCmdCopyImage between D32 and D32S8 is illegal even for the depth
// aspect, the game DOES use stencil volumes every frame in the WM30 venues / ladder / six-man scenes, the format is part of the pass and pipeline keys (a promotion rebuilds the pass family), and
// Gen9 keeps stencil as a separate surface (the depth plane of D32S8 is the same as D32). What stays: the census, and this per-frame lever.
//
// wwe2k14_rdr_stencil_dead (PerFrame, default off): what the game does with a depth image's stencil in a frame is often only WRITING it (the library Clear clears depth + stencil together, every
// depth pass). A write nothing reads is work removed:
//   (a) an absorbed full stencil CLEAR (the pass's stencil load op) becomes DONT_CARE when no draw of the pass tests stencil and no later pass or copy reads the stencil;
//   (b) a stencil clear inside a pass is not a USE (it sets no stencil_used): the load / store of stencil die across passes when nothing reads, and the clear command itself drops its stencil
//       aspect when nothing reads;
//   (c) a depth copy takes the DEPTH aspect only when the stencil is dead at its destination (no later pass tests it, no later copy carries it on); otherwise both aspects, as before.
// Load / store ops are not part of render-pass compatibility: no pipeline is rebuilt.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rdr {

// --wwe2k14_rdr_stencil_dead (default OFF, PerFrame: live.cpp stores it before Render). fview / rplay: --stencil-dead 1.
inline std::atomic<bool> g_stencil_dead{false};
// Tests only (--stencil-dead-plant N), 0 = off: 1 an absorbed stencil clear is dropped even when a draw of the pass tests stencil (a stencil clear wrongly dropped before a stencil test: the stencil
// picture must change); 2 every depth copy takes the depth aspect only (a copy that drops a live stencil); 3 an in-pass stencil clear loses its aspect even when a later pass reads it;
// 4 a copy that keeps both aspects does not mark its source needed (only the poison arm and the oracle see it).
inline std::atomic<int> g_stencil_dead_plant{0};
// Tools only (--stencil-dead-poison): the lever's removed work leaves GARBAGE instead of stale-but-right bytes (a 4090 keeps the old stencil under DONT_CARE, so hash equality cannot see a stencil
// the lever wrongly declared dead; Adreno and maybe the UHD 620 would): every stencil load the lever turns into DONT_CARE becomes CLEAR 0xA5, a stripped in-pass clear clears to 0xA5 instead of being
// skipped, a pass whose stencil store is DONT_CARE clears the stencil to 0xA5 just before it ends, and a depth-only copy is preceded by a stencil clear to 0xA5 of its destination.
inline std::atomic<bool> g_stencil_dead_poison{false};
// The forward oracle (census runs): every pass that tests stencil must see a DEFINED stencil (cleared, loaded from a defined one, or carried by a copy); counted per run.
inline std::atomic<uint64_t> g_sd_oracle_tests{0}, g_sd_oracle_violations{0};
// The lever's live census (whole run): frames with it on, absorbed clears made DONT_CARE / kept, in-pass clears stripped / kept, depth copies taken depth-only / kept whole.
inline std::atomic<uint64_t> g_sd_frames{0}, g_sd_abs_dropped{0}, g_sd_abs_kept{0}, g_sd_in_stripped{0}, g_sd_in_kept{0}, g_sd_copy_depth_only{0}, g_sd_copy_whole{0}, g_sd_loads_dead{0};

constexpr uint32_t kDepthCensusSlots = 16384;   // image ids >= this share the last slot
struct DepthCensusSlot {
  std::atomic<uint32_t> width{0}, height{0};
  std::atomic<uint64_t> draws{0};                   // draws with a depth target in this image
  std::atomic<uint64_t> stencil_test_draws{0};      // draws with the stencil test enabled
  std::atomic<uint64_t> stencil_effect_draws{0};    // ... whose test is not ALWAYS or whose ops are not all KEEP (a real use)
  std::atomic<uint64_t> stencil_write_draws{0};     // ... whose ops change the stencil
  std::atomic<uint64_t> stencil_clears{0};          // clears naming the stencil aspect
  std::atomic<uint64_t> stencil_clears_nonzero{0};  // ... with a value other than 0
  std::atomic<uint32_t> use_classes{0};             // bit mask of GpuClass (1 << class) of the first colour target of draws that used stencil for real
};
inline DepthCensusSlot g_depth_census[kDepthCensusSlots];
inline std::atomic<bool> g_depth_census_on{false};   // set by fview / rplay (--depth-census): the live game never pays for it
inline DepthCensusSlot& DepthCensusFor(uint32_t image) { return g_depth_census[image < kDepthCensusSlots ? image : kDepthCensusSlots - 1]; }

// VkCompareOp ALWAYS = 7, VkStencilOp KEEP = 0 (api.h StencilFace holds the Vulkan values). A face with ALWAYS + KEEP / KEEP / KEEP neither tests nor writes anything.
struct StencilFaceView { uint8_t fail, pass, depth_fail, compare; };
inline bool FaceHasEffect(const StencilFaceView& f) { return f.compare != 7 || f.fail != 0 || f.pass != 0 || f.depth_fail != 0; }
inline bool FaceWrites(const StencilFaceView& f) { return f.fail != 0 || f.pass != 0 || f.depth_fail != 0; }

// The rule for one draw (census): does its stencil state have an effect? (The lever itself is stricter: any draw with the test ENABLED counts as a use, as the renderer always did.)
inline bool DrawNeedsStencil(bool stencil_test, const StencilFaceView& front, const StencilFaceView& back) {
  return stencil_test && (FaceHasEffect(front) || FaceHasEffect(back));
}

// The census as lines (fview / rplay print them): one per depth image that saw a draw, then the verdict line the gate reads.
inline std::vector<std::string> DepthCensusLines() {
  std::vector<std::string> out;
  uint32_t images = 0, used = 0, tested = 0, nonzero = 0, cleared = 0;
  char buf[420];
  for (uint32_t i = 0; i < kDepthCensusSlots; ++i) {
    const DepthCensusSlot& s = g_depth_census[i];
    const uint64_t draws = s.draws.load();
    if (!draws && !s.stencil_clears.load()) continue;
    ++images;
    const bool real = s.stencil_effect_draws.load() != 0 || s.stencil_clears_nonzero.load() != 0;
    used += real ? 1 : 0;
    tested += (s.stencil_test_draws.load() != 0 && !real) ? 1 : 0;
    nonzero += s.stencil_clears_nonzero.load() != 0 ? 1 : 0;
    cleared += s.stencil_clears.load() != 0 ? 1 : 0;
    std::snprintf(buf, sizeof buf, "[r24c] depth image %u %ux%u: draws %llu, stencil test %llu, real effect %llu, writes %llu, clears %llu (nonzero %llu), classes 0x%X%s", i, s.width.load(), s.height.load(),
                  static_cast<unsigned long long>(draws), static_cast<unsigned long long>(s.stencil_test_draws.load()), static_cast<unsigned long long>(s.stencil_effect_draws.load()),
                  static_cast<unsigned long long>(s.stencil_write_draws.load()), static_cast<unsigned long long>(s.stencil_clears.load()), static_cast<unsigned long long>(s.stencil_clears_nonzero.load()),
                  s.use_classes.load(), real ? " STENCIL USED" : "");
    out.push_back(buf);
  }
  std::snprintf(buf, sizeof buf, "[r24c] census: depth images %u, stencil really used on %u, stencil-tested with no effect on %u, stencil cleared on %u, non-zero stencil clears on %u", images, used, tested, cleared, nonzero);
  out.push_back(buf);
  std::snprintf(buf, sizeof buf, "[r24c] stencil_dead: %s, plant %d, frames %llu, absorbed clears dropped %llu kept %llu, in-pass clears stripped %llu kept %llu, depth copies depth-only %llu whole %llu, stencil loads dead %llu, poison %d, oracle: stencil-testing passes %llu, undefined at the test %llu",
                g_stencil_dead.load() ? "on" : "off", g_stencil_dead_plant.load(), static_cast<unsigned long long>(g_sd_frames.load()), static_cast<unsigned long long>(g_sd_abs_dropped.load()),
                static_cast<unsigned long long>(g_sd_abs_kept.load()), static_cast<unsigned long long>(g_sd_in_stripped.load()), static_cast<unsigned long long>(g_sd_in_kept.load()),
                static_cast<unsigned long long>(g_sd_copy_depth_only.load()), static_cast<unsigned long long>(g_sd_copy_whole.load()), static_cast<unsigned long long>(g_sd_loads_dead.load()), int(g_stencil_dead_poison.load()), static_cast<unsigned long long>(g_sd_oracle_tests.load()), static_cast<unsigned long long>(g_sd_oracle_violations.load()));
  out.push_back(buf);
  return out;
}

}  // namespace rdr
