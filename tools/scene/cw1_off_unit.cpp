// OFF-BUILD UNIT (S103 CW1 review fix 1): the stream-free build's view of the CAW kind. Built with -DWWE2K14_KEEP_STREAM=0 (tools\scene\l2d_check.py builds it so; the static_assert below
// refuses any other build, and tools\scene\off_unit_lint.py fails a unit that carries the OFF-BUILD UNIT marker without it).
// Why a separate unit: l2d_unit.cpp is a stream-build unit (its DecideNow gates on inputs). In the OFF build DecideNow ignores inputs; the ONLY thing the Caw mask changes there is the never_fed hold:
// scene_host.cpp calls SceneFeed::Note(RequiredFor(kind, registered), fed_now, stamped_ever) (host_core.h kFeedStructural). Of the bits kCawDrop drops, only ring_rows (6) is structural.
//   off.caw_feed       Caw: a scene fed everything but ring_rows (and never crowd / plane) is NOT held; Match with the same feed IS held on bit 6
//   off.caw_kept       Caw still holds a missing screens (14) / pools (15) / arena (5) / camera (12) / palette (3) / arena_models (16)
//   off.caw_flag_off   with g_caw_enabled off (--wwe2k14_rdr_scene_caw=false) Caw is Match's mask: bit 6 holds
//   off.match_entrance Match / Entrance masks hold bit 6 as before
// Planted twin: --plant 1 = the feed rule ignores the kind (Note gets the registered mask): off.caw_feed must fail.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/keep_stream.h"
static_assert(!WWE2K14_STREAM(), "cw1_off_unit.cpp is an OFF-build unit: build it with -DWWE2K14_KEEP_STREAM=0 (its claims are about the stream-free build)");
#include "../../src/scene_live/inputs_core.h"
#include "../../src/renderer_host/host_core.h"

namespace sk = scene_kind;
namespace si = scene_inputs;
namespace sh = wwe2k14::scene_host;

static int g_fail = 0;
#define CHECK(name, c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s (line %d): %s\n", name, __LINE__, #c); } } while (0)
static int g_plant = 0;

// One scene's first frame: `fed` = the bits stamped since the scene began, every bit stamped somewhere this session.
static uint32_t Held(sk::Kind k, uint32_t reg, uint32_t fed) {
  sh::SceneFeed feed;
  feed.NewScene(1);
  const uint32_t all = (1u << si::kInputCount) - 1u;
  return feed.Note(g_plant == 1 ? reg : sk::RequiredFor(k, reg), fed, all);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) g_plant = std::atoi(argv[i + 1]);
  si::RegisterAll();
  const uint32_t reg = rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load();
  const uint32_t bit6 = 1u << si::kInRingRows;
  sk::g_caw_enabled.store(true);
  CHECK("off.caw_feed", Held(sk::Kind::Caw, reg, reg & ~bit6) == 0);                                   // the editor never feeds the ring rows: no hold
  CHECK("off.caw_feed", Held(sk::Kind::Caw, reg, reg & ~bit6 & ~(1u << si::kInCrowdSnapshot) & ~(1u << si::kInPlaneConstants)) == 0);
  CHECK("off.match_entrance", Held(sk::Kind::Match, reg, reg & ~bit6) == bit6);                        // match / entrance still hold on bit 6
  CHECK("off.match_entrance", Held(sk::Kind::Entrance, reg, reg & ~bit6) == bit6);
  const int kept[] = {si::kInStageScreens, si::kInStagePools, si::kInArenaWorlds, si::kInCamera, si::kInWrestlerPalette, si::kInArenaModels};
  for (int b : kept) CHECK("off.caw_kept", Held(sk::Kind::Caw, reg, reg & ~(1u << b)) == (1u << b));   // structural and fed in the editor: still held when missing
  sk::g_caw_enabled.store(false);
  CHECK("off.caw_flag_off", Held(sk::Kind::Caw, reg, reg & ~bit6) == bit6);                            // --wwe2k14_rdr_scene_caw=false: the dev knock-out
  sk::g_caw_enabled.store(true);
  if (g_fail) std::printf("cw1_off_unit: %d FAILED\n", g_fail);
  else std::printf("cw1_off_unit: all checks passed\n");
  return g_fail ? 1 : 0;
}
