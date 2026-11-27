// S102 lane S0-1: the Surface bench's settings profiles, from the game's own rules (src\gfx_applied.h, src\gfx_settings.h).
//   gfx_applied --fresh --gpu integrated [--gpu-name NAME] [--tier 0|2] [--frame-rate N] --out FILE
//       writes the settings a fresh install on that kind of card gets (launcher first run: SuggestIntegrated, AUTO tier, LowStartValues);
//       --tier 2 = the same install after the player picked STANDARD (the bench's standard_player profile).
//       --frame-rate overrides the file's frame rate. A fresh install on integrated graphics writes 3 (30 fps everywhere); the bench's 1 is
//       NOT just a number: frame_rate 3 also asks for wwe2k14_entrance_60fps=false, wwe2k14_step_hz=30 (+ step_hz_entrance=60: in owner mode the
//       game keeps 60 updates and draws one picture per two vblanks, reader/start_live.inc (wwe2k14::rdr_reader::StartLive): 30 pictures over 60 logic updates),
//       gpu_vblank_precise_timer_spin_us=250 and vulkan_texture_load_direct_unscaled (gfx_settings.h (wwe2k14::gfxs::GraphicsFlags)). frame_rate 1 leaves all four out.
//   gfx_applied --truth-table [--planted]
//       ResolveLow against a frozen copy of the pre-S0-1 inline block in gfx_menu.h, over tier 0-2 x phone x integrated x original_look x
//       renderer 0-2 x renderer flag -1..2; prints `truth table: N cases, M mismatches` (exit 1 when M > 0). --planted breaks the frozen copy
//       on purpose (it must then report mismatches).
//   gfx_applied --settings FILE --gpu integrated|discrete|phone [--gpu-name NAME] [--panel WxH] [--renderer-flag native|light|accurate]
//       prints the `[gfx][applied]` line the game logs at boot for that settings file on that kind of card (exit 0), or why it cannot (exit 2)
// Offline: no game, no GPU. The game's own parse (gfxs::Parse), tier resolution (ResolveLow) and flag list (GraphicsFlags) are what runs.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/gfx_applied.h"

using namespace wwe2k14;

static const char* Arg(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], name)) return argv[i + 1];
  return nullptr;
}
static bool Has(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], name)) return true;
  return false;
}

// The block gfx_menu.h had before S0-1, frozen (do not edit: it is the reference the shared function is compared with).
static bool OldInlineLow(int tier, bool phone, bool integrated, const std::string& name, bool original_look, int renderer, int flag,
                         std::string* why, bool planted) {
  std::string text;
  bool low = gfxs::ResolveQualityTier(tier, phone, integrated, name, &text) == 1;
  const bool flag_blocks = planted ? false : (flag >= 0 && flag != 2);  // planted: the renderer flag is forgotten
  if (low && (original_look || renderer != 2 || flag_blocks)) {
    low = false;
    text = original_look ? "STANDARD (original look is on)" : "STANDARD (not the NATIVE renderer)";
  }
  *why = text;
  return low;
}

static int TruthTable(bool planted) {
  int cases = 0, bad = 0;
  for (int tier = 0; tier <= 2; ++tier)
    for (int phone = 0; phone <= 1; ++phone)
      for (int integrated = 0; integrated <= 1; ++integrated)
        for (int orig = 0; orig <= 1; ++orig)
          for (int renderer = 0; renderer <= 2; ++renderer)
            for (int flag = -1; flag <= 2; ++flag) {
              gfxs::Choice c;
              c.quality_tier = tier, c.original_look = orig != 0, c.renderer = renderer;
              std::string now, old;
              gfxs::ResolveLow(c, phone != 0, integrated != 0, "GPU", flag, &now);
              const bool old_low = OldInlineLow(tier, phone != 0, integrated != 0, "GPU", orig != 0, renderer, flag, &old, planted);
              ++cases;
              if (c.low != old_low || now != old) ++bad;
            }
  std::printf("truth table: %d cases, %d mismatches\n", cases, bad);
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  if (Has(argc, argv, "--truth-table")) return TruthTable(Has(argc, argv, "--planted"));
  const char* gpu = Arg(argc, argv, "--gpu");
  if (!gpu) return std::fprintf(stderr, "gfx_applied: --gpu integrated|discrete|phone is required\n"), 2;
  const bool integrated = !std::strcmp(gpu, "integrated"), phone = !std::strcmp(gpu, "phone");
  if (!integrated && !phone && std::strcmp(gpu, "discrete")) return std::fprintf(stderr, "gfx_applied: unknown --gpu %s\n", gpu), 2;
  const char* name_arg = Arg(argc, argv, "--gpu-name");
  const std::string gpu_name = name_arg ? name_arg : integrated ? "Intel(R) UHD Graphics 620" : "NVIDIA GeForce RTX 4090";

  if (Has(argc, argv, "--fresh")) {
    const char* out = Arg(argc, argv, "--out");
    if (!out) return std::fprintf(stderr, "gfx_applied: --fresh needs --out FILE\n"), 2;
    std::string what;
    const char* tier_arg = Arg(argc, argv, "--tier");
    gfxs::Choice c = gfxs::FreshInstall(integrated, gpu_name, &what, tier_arg ? std::atoi(tier_arg) : 0);
    if (const char* fr = Arg(argc, argv, "--frame-rate")) c.frame_rate = std::atoi(fr);
    const std::string json = gfxs::FreshJson(c);
    if (!gfxs::WriteJson(out, json)) return std::fprintf(stderr, "gfx_applied: cannot write %s\n", out), 2;
    std::printf("wrote %s (%s)\n", out, what.c_str());
    return 0;
  }

  const char* file = Arg(argc, argv, "--settings");
  if (!file) return std::fprintf(stderr, "gfx_applied: --settings FILE (or --fresh) is required\n"), 2;
  const std::string json = gfxs::ReadFile(file);
  if (json.empty()) return std::fprintf(stderr, "gfx_applied: cannot read %s (or it is empty)\n", file), 2;
  gfxs::Choice c;
  gfxs::Parse(json, c);
  const char* rf = Arg(argc, argv, "--renderer-flag");
  const int renderer_flag = gfxs::RendererFromFlags(rf ? std::string("--wwe2k14_renderer=") + rf : std::string());
  // (gfx_menu.h: the integrated flag is only asked of the card while the setting is AUTO.)
  std::string why;
  gfxs::ResolveLow(c, phone, c.quality_tier == 0 ? integrated : false, gpu_name, renderer_flag, &why);
  if (renderer_flag >= 0) c.renderer = renderer_flag;
  const auto flags = gfxs::GraphicsFlags(c, std::string(), 60);
  int sw = 0, sh = 0;
  if (const char* panel = Arg(argc, argv, "--panel")) std::sscanf(panel, "%dx%d", &sw, &sh);
  std::printf("%s\n", gfxs::AppliedLine(c, flags, why, sw, sh).c_str());
  return 0;
}
