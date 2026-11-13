// Session 101 lane PN: runs the SHIPPED post-name path (renderer_adapter\post_origin.h PostSiteScope / ApplyPostSite, then names_draw.h
// TakeDrawOriginInto and the generated origin_table.h) the way the game's threads do: a queue-function hook opens a scope with the effect's LR,
// the op-68 recorder asks for its key, the replay turns the tag into a DrawOrigin. No game, no SDK. Built and run by tools\names\post_names_selftest.py.
//   post_origin_test.exe            checks; exit 0 = all pass
//   built with -DS101_PLANT_POST_DROP_LR (the hook forgets the LR): the same checks must FAIL (exit 1)
#include <cstdio>
#include <string>
#include <unordered_map>

#include "names_draw.h"
#include "post_origin.h"

namespace og = rdr_adapter::origin;
namespace nd = rdr_adapter::names_draw;

static int failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}
static const char* At(const std::string& strings, uint32_t name) { return name ? strings.c_str() + (name - 1) : ""; }

struct Site { uint32_t call; const char* technique; rdr_adapter::DrawClass klass; };

// One post quad: the hook is entered with LR = call + 4, the op-68 recorder (called from the generic queue site 0x82C1F48C) builds the key.
static rdr_adapter::DrawOrigin Quad(std::string& strings, std::unordered_map<const char*, uint32_t>& map, uint32_t effect_call, uint32_t queue_site, bool in_scope) {
  og::Key k;
  k.op = 68;
  if (in_scope) {
    og::PostSiteScope scope(effect_call + 4);  // the hook on sub_82C1F420 / 4C0 / 610 around the original
    og::ApplyPostSite(k, 68, queue_site);      // inside it: the recorder's RecordTag
  } else {
    og::ApplyPostSite(k, 68, queue_site);
  }
  og::t_replay_tag = og::Global().Intern(k);
  og::t_replay_op = 68;
  return nd::TakeDrawOriginInto(strings, map);
}

int main() {
  og::g_on = true;
  using rdr_adapter::DrawClass;
  // The 22 queue sites of sub_82C1F5E8 (q callers) and their effect function's technique, plus the direct callers of 82C1F420.
  const Site sites[] = {
      {0x82C1CBC4, "T_CalculateAdaptedLum", DrawClass::Post}, {0x82C1CD9C, "T_DownScale2x2DownColor", DrawClass::Post},
      {0x82C1CF64, "T_Bloom", DrawClass::Post}, {0x82C1D098, "T_Bloom", DrawClass::Post}, {0x82C1D1B4, "T_Bloom", DrawClass::Post},
      {0x82C1D748, "T_Star", DrawClass::Post}, {0x82C1D950, "T_Star", DrawClass::Post},
      {0x82C1DE38, "T_SampleLum", DrawClass::Post}, {0x82C1DF50, "T_SampleLum", DrawClass::Post}, {0x82C1E064, "T_SampleLum", DrawClass::Post}, {0x82C1E15C, "T_SampleLum", DrawClass::Post},
      {0x82C28E08, "T_RetroFilm", DrawClass::Post}, {0x82C29210, "T_Emboss", DrawClass::Post}, {0x82C2960C, "T_PostEffectDefocusAdjustHue", DrawClass::Post},
      {0x82C29B60, "T_PostEffectHalation", DrawClass::Post}, {0x82C29C54, "T_PostEffectHalation", DrawClass::Post}, {0x82C2A06C, "T_PostEffectHalation", DrawClass::Post}, {0x82C2A3B4, "T_PostEffectHalation", DrawClass::Post},
      {0x82C2A82C, "T_PostEffectMonotone", DrawClass::Post}, {0x82C2B81C, "T_GaussianBlur", DrawClass::Post}, {0x82C2BB58, "T_DownScale2x2_DOF", DrawClass::Post}, {0x82C2C1B0, "T_Antialiasing", DrawClass::Post},
      // direct callers of 82C1F420 / the callers of 82C1F610 and 82C1F4C0
      {0x82C1F9BC, "T_FillColorDepth", DrawClass::Post}, {0x82C1DA38, "T_Final", DrawClass::Post},
      {0x827E8C5C, "T_ShadowMapBlur", DrawClass::Shadow}, {0x827EA3C8, "T_GaussianBlur5x5", DrawClass::Shadow}, {0x82813E14, "T_EffectMask_DownScale", DrawClass::Shadow},
  };
  std::string strings;
  std::unordered_map<const char*, uint32_t> map;
  int named = 0, total = 0;
  for (const Site& s : sites) {
    const uint32_t queue_site = (s.call == 0x82C1DA38) ? 0x82C1F674u : 0x82C1F48Cu;
    const rdr_adapter::DrawOrigin o = Quad(strings, map, s.call - 4, queue_site, true);
    ++total;
    const bool ok = o.call_site == s.call - 4 && o.part == queue_site && o.klass == s.klass && std::string(At(strings, o.technique)) == s.technique;
    named += ok;
    if (!ok) std::printf("      site %08X: call_site %08X part %08X class %d technique '%s' (want %s)\n", s.call, o.call_site, o.part, int(o.klass), At(strings, o.technique), s.technique);
  }
  Check(named == total, "every queue site (22 of sub_82C1F5E8 + the direct callers) names its effect function and technique");
  // The generic queue site, with no effect scope (a callback queued by something we do not hook), keeps its old name: nothing is invented.
  const rdr_adapter::DrawOrigin g = Quad(strings, map, 0, 0x82C1F48C, false);
  Check(g.call_site == 0x82C1F48C && g.part == 0, "outside a scope the quad keeps the generic queue site");
  // Leaving the scope clears the state: a quad after the hook returned is not given the last effect's name.
  Check(og::t_post_site == 0, "the scope restores the thread state when the hook returns");
  // Origins off: the hook writes nothing and the recorder reads nothing.
  og::g_on = false;
  {
    og::PostSiteScope scope(0x82C1CBC8);
    Check(og::t_post_site == 0, "origins off: the scope sets nothing (cost: one relaxed load)");
  }
  og::g_on = true;
  // Only op 68 takes the name: an op-15 record on this thread inside a scope keeps its own call site.
  {
    og::PostSiteScope scope(0x82C1CBC8);
    og::Key k;
    og::ApplyPostSite(k, 15, 0x82518950);
    Check(k.call_site == 0x82518950, "an op-15 record inside a scope is not renamed");
  }
  std::printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
