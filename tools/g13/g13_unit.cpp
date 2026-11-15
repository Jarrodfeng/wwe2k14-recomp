// S102 lane G13: the draw heights between 540 and 720 (576 / 600 / 648), NATIVE only, resolution 0.
// Built by tools\g13\g13_check.py against the real src\gfx_settings.h / src\gfx_applied.h (and against planted twins that must FAIL it).
//   exit 0 = every expectation holds; prints one line per failed expectation.
// With --list prints the entries the launcher offers (NATIVE, then not NATIVE), in its own order and labels.
#include <cstdio>
#include <cstring>
#include <string>

#include "gfx_applied.h"
#include "gfx_settings.h"

using namespace wwe2k14;
static int fails = 0;
static void Expect(bool ok, const char* what) {
  if (!ok) std::printf("FAIL %s\n", what), ++fails;
}

int main(int argc, char** argv) {
  if (argc > 1 && !std::strcmp(argv[1], "--list")) {
    for (int native = 1; native >= 0; --native) {
      std::printf(native ? "launcher list, NATIVE:" : "launcher list, not NATIVE:");
      for (int i = 0; i < gfxs::kResolutionCount; ++i)
        if (native || !gfxs::kResolutions[i].native_only) std::printf(" %s", gfxs::kResolutions[i].name);
      std::printf("\n");
    }
    return 0;
  }
  // The heights the settings key accepts: resolution 0 takes 360 / 480 / 540 / 576 / 600 / 648, resolution 1 only 1080, nothing else.
  for (int h : {360, 480, 540, 576, 600, 648}) Expect(gfxs::ValidDrawHeight(0, h) == h, "resolution 0 accepts the draw height");
  Expect(gfxs::ValidDrawHeight(1, 1080) == 1080, "1440p accepts 1080");
  Expect(gfxs::ValidDrawHeight(1, 600) == 0, "600 is NOT accepted for 1440p");
  Expect(gfxs::ValidDrawHeight(1, 648) == 0, "648 is NOT accepted for 1440p");
  Expect(gfxs::ValidDrawHeight(1, 576) == 0, "576 is NOT accepted for 1440p");
  Expect(gfxs::ValidDrawHeight(2, 600) == 0, "600 is NOT accepted for 4K");
  Expect(gfxs::ValidDrawHeight(0, 1080) == 0, "1080 is NOT accepted for 720p");
  Expect(gfxs::ValidDrawHeight(0, 720) == 0, "720 is the whole size (draw_height 0), not a draw height");
  Expect(gfxs::ValidDrawHeight(0, 601) == 0 && gfxs::ValidDrawHeight(0, 649) == 0 && gfxs::ValidDrawHeight(0, 0) == 0, "an unlisted height is 0");

  // The menu entry of a pair, with NATIVE and without.
  for (int h : {360, 480, 540, 576, 600, 648}) {
    const int i = gfxs::ResolutionIndex(0, h, true);
    Expect(gfxs::kResolutions[i].draw_height == h && gfxs::kResolutions[i].resolution == 0, "ResolutionIndex finds the draw height's entry");
    Expect(gfxs::kResolutions[i].native_only, "a draw height entry is NATIVE only");
    const int w = gfxs::ResolutionIndex(0, h, false);
    Expect(gfxs::kResolutions[w].resolution == 0 && gfxs::kResolutions[w].draw_height == 0, "not NATIVE shows the whole size (720P)");
  }
  Expect(gfxs::ResolutionIndex(1, 600, true) == gfxs::ResolutionIndex(1, 0, true), "1440p with 600 shows as 1440P");
  Expect(std::string(gfxs::kResolutions[gfxs::ResolutionIndex(1, 0, true)].name) == "1440P", "the 1440P fallback is by name");
  Expect(std::string(gfxs::kResolutions[gfxs::ResolutionIndex(7, 0, true)].name) == "1440P", "a resolution nobody lists (7) shows 1440P");
  Expect(std::string(gfxs::kResolutions[gfxs::ResolutionIndex(0, 601, true)].name) == "720P", "an unlisted height shows the whole size, 720P");
  // Order: ascending within a resolution, labels unique.
  int last = 0;
  for (int i = 0; i < gfxs::kResolutionCount; ++i) {
    const auto& e = gfxs::kResolutions[i];
    if (e.resolution == 0) {
      const int rows = e.draw_height ? e.draw_height : 720;
      Expect(rows > last, "resolution 0 entries ascend");
      last = rows;
    }
    for (int j = i + 1; j < gfxs::kResolutionCount; ++j) Expect(std::strcmp(e.name, gfxs::kResolutions[j].name) != 0, "labels are unique");
  }
  Expect(gfxs::kResolutionCount <= 32, "the in-game text ids (kTextVal2 = 0x30 up to kTextVal3 = 0x50: 32 slots) hold every entry");
  int in_game_native = 0;
  for (int i = 0; i < gfxs::kResolutionCount; ++i) in_game_native += gfxs::kResolutions[i].in_game_menu ? 1 : 0;
  Expect(in_game_native <= 7, "the in-game row (8 slots) still has room for the one launcher-only entry the file holds");

  {  // The list the launcher offers without NATIVE: the whole sizes only, in this order.
    const char* want[] = {"720P", "1440P", "4K"};
    int n = 0;
    bool same = true;
    for (int i = 0; i < gfxs::kResolutionCount; ++i)
      if (!gfxs::kResolutions[i].native_only) same = same && n < 3 && !std::strcmp(gfxs::kResolutions[i].name, want[n++]);
    Expect(same && n == 3, "without NATIVE the launcher lists 720P, 1440P, 4K only");
  }
  // The settings file round trip and the flag the game gets (no default changed: a file without the key reads 0).
  for (int h : {576, 600, 648}) {
    gfxs::Choice c;
    char json[128];
    std::snprintf(json, sizeof(json), "{\"renderer\":2,\"resolution\":0,\"draw_height\":%d,\"upscale\":0}", h);
    gfxs::Parse(json, c);
    Expect(c.draw_height == h, "a settings file with the height keeps it");
    const auto flags = gfxs::GraphicsFlags(c, std::string(), 60);
    std::string got;
    for (const auto& f : flags)
      if (f.first == "gpu_native_truth_draw_height") got = f.second;
    Expect(got == std::to_string(h), "gpu_native_truth_draw_height carries the height");
    Expect(gfxs::InternalSize(c).second == h, "the applied line internal height");
  }
  gfxs::Choice d;
  Expect(d.draw_height == 0 && d.resolution == 0, "the default stays 720p, no draw height");
  return fails ? 1 : 0;
}
