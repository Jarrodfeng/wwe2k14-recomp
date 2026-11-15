// S102 M7: offline test of the in-run A/B flip's parser and schedule (src/ab_flip_math.h). 0 game runs.
//   clang++ -std=c++20 -Wall -Wextra -Werror -I src tools/ab_flip/ab_flip_test.cpp
// Prints PASS / FAIL lines and "all checks passed" (exit 0) or the failing count (exit 1).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ab_flip_math.h"

static int g_fail = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static std::vector<std::string> g_sets, g_logs;
static bool g_reject = false;
static bool FakeSet(const char* n, const char* v) { g_sets.push_back(std::string(n) + "=" + v); return !g_reject; }
static void FakeLog(const char* l) { g_logs.push_back(l); }
static std::string g_start_value = "false";  // what the fake registry holds for every cvar at start
static bool FakeRead(const char*, std::string& t) { t = g_start_value; return true; }
// nf_replay_loop / nf_render_dispatch / prelight are bool, max_queued is an int
static bool FakeIsBool(const char* n) { return std::strcmp(n, "wwe2k14_rdr_max_queued") != 0; }

static const char* kL = "wwe2k14_nf_replay_loop";
static const char* kGood = "period=300/settle=30/off=wwe2k14_nf_replay_loop=0+wwe2k14_nf_render_dispatch=0/on=wwe2k14_nf_replay_loop=1+wwe2k14_nf_render_dispatch=1";

static ab_flip::Env MakeEnv() {
  ab_flip::Env e;
  e.is_bool = &FakeIsBool;
  e.read = &FakeRead;
  return e;
}

static bool Refused(const std::string& spec, const char* needle, bool with_env = true) {
  ab_flip::Env e = MakeEnv();
  ab_flip::Spec s = ab_flip::Parse(spec, with_env ? &e : nullptr);
  return !s.ok && s.error.find(needle) != std::string::npos;
}

int main() {
  using namespace ab_flip;
  Spec s = Parse(kGood);
  Check(s.ok && s.period == 300 && s.settle == 30 && s.phases.size() == 2 && s.order.size() == 2 && s.phases[0].name == "off", "good spec parses");
  Check(s.phases[1].sets.size() == 2 && s.phases[1].sets[0].second == "1", "phase on has both settings");
  // refusals: the whole spec, with the reason
  Check(Refused("period=300/settle=30/off=wwe2k14_rdr_copy_alias=0/on=wwe2k14_rdr_copy_alias=1", "start-only"), "a start-only switch is refused");
  Check(Refused("period=300/off=wwe2k14_no_such_thing=0/on=wwe2k14_no_such_thing=1", "not in the switch table"), "a name outside the table is refused");
  Check(Refused("period=300/off=wwe2k14_nf_replay_loop=maybe/on=wwe2k14_nf_replay_loop=1", "not true / false"), "a bad value is refused");
  Check(Refused("period=300/off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=2", "bool setting takes 0 / 1"), "a bool set to 2 is refused (the registry would read false)");
  Check(!Refused("period=300/off=wwe2k14_rdr_max_queued=1/on=wwe2k14_rdr_max_queued=3", "bool"), "an int setting may take 3");
  Check(!Refused("off=wwe2k14_nf_replay_loop=false/on=wwe2k14_nf_replay_loop=true", "x"), "true / false are accepted for a bool");
  Check(Refused("period=1/off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1", "period"), "period 1 is refused");
  Check(Refused("period=30/settle=30/off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1", "settle"), "settle >= period is refused");
  Check(Refused("off=wwe2k14_nf_replay_loop=0", "two phases"), "one phase is refused");
  Check(Refused("", "empty"), "an empty spec is refused");
  Check(Refused("off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_render_dispatch=1", "does not"), "phases that set different switches are refused");
  Check(Refused("off=wwe2k14_nf_replay_loop=0/off=wwe2k14_nf_replay_loop=1", "twice"), "a phase written twice is refused");
  Check(Refused("off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1/order=off:zzz", "not a phase"), "an order naming a missing phase is refused");
  Check(Refused("off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1/period=abc", "number"), "a non-numeric period is refused");
  Check(Refused("anchor=0/off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1", "anchor"), "anchor 0 is refused");
  // armed switches: the start value must already be the non-reference phase's
  g_start_value = "false";
  Check(Refused("period=100/off=wwe2k14_rdr_prelight_layered=0/on=wwe2k14_rdr_prelight_layered=1", "armed at start"), "an armed switch that starts OFF is refused");
  g_start_value = "true";
  Check(!Refused("period=100/off=wwe2k14_rdr_prelight_layered=0/on=wwe2k14_rdr_prelight_layered=1", "x"), "an armed switch that starts on (the non-reference value) is accepted");
  Check(!Refused("period=100/off=wwe2k14_rdr_prelight_layered=0/on=wwe2k14_rdr_prelight_layered=0", "x"), "an A/A spec on an armed switch is accepted");
  g_start_value = "false";
  Check(Refused("period=100/off=wwe2k14_rdr_card_cut=0/on=wwe2k14_rdr_card_cut=true", "armed at start"), "an armed switch compared by value, true == 1: still refused when it starts false");
  // a mixed spec (one good, one start-only switch) refuses WHOLE: nothing applied
  {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin("off=wwe2k14_nf_replay_loop=0+wwe2k14_rdr_copy_alias=0/on=wwe2k14_nf_replay_loop=1+wwe2k14_rdr_copy_alias=1", 1, FakeLog);
    for (uint64_t f = 1; f < 1000; ++f) r.AtFrame(f, 0, FakeSet, FakeLog);
    Check(!r.active && g_sets.empty() && g_logs.size() == 1 && g_logs[0].find("REFUSED") != std::string::npos, "a mixed spec is refused whole: no setting written, one REFUSED line");
  }
  // schedule, no anchor: phase 0 at frame 2 (frame 1's refresh is silent)
  {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin(kGood, 1, FakeLog);
    std::vector<uint64_t> at;
    for (uint64_t f = 1; f <= 1500; ++f) if (r.AtFrame(f, 0, FakeSet, FakeLog)) at.push_back(f);
    Check(at.size() == 5 && at[0] == 2 && at[1] == 302 && at[2] == 602 && at[4] == 1202, "flips at frames 2, 302, 602, 902, 1202 (never at 1)");
    Check(g_sets.size() == 10 && g_sets[0] == "wwe2k14_nf_replay_loop=0" && g_sets[2] == "wwe2k14_nf_replay_loop=1" && g_sets[4] == "wwe2k14_nf_replay_loop=0", "phases alternate off, on, off");
    Check(g_logs.size() == 7 && g_logs[2] == "[ab_flip] frame 2 phase off no 0 rel 2" && g_logs[3] == "[ab_flip] frame 302 phase on no 1 rel 302", "one log line per phase change, with the guest frame and rel");
    Check(!r.AtFrame(1500, 0, FakeSet, FakeLog), "the same phase does not flip twice");
  }
  // anchor: nothing but the reference before the mark; phases follow the MARK's frame, not the absolute frame
  for (uint64_t mark : {uint64_t(500), uint64_t(530)}) {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin("period=100/anchor=2/start=0/off=wwe2k14_nf_replay_loop=0/on=wwe2k14_nf_replay_loop=1", 1, FakeLog);
    std::vector<uint64_t> at;
    for (uint64_t f = 1; f < 1000; ++f) if (r.AtFrame(f, f >= mark ? mark : 0, FakeSet, FakeLog)) at.push_back(f);
    Check(at.size() == 6 && at[0] == 2 && at[1] == mark && at[2] == mark + 100 && at[5] == mark + 400, "anchor: reference at frame 2, then flips at the mark frame + k * period (mark " + std::to_string(mark) + ")");
    Check(g_sets[0] == "wwe2k14_nf_replay_loop=0" && g_sets[1] == "wwe2k14_nf_replay_loop=0" && g_sets[2] == "wwe2k14_nf_replay_loop=1", "the reference values run before the anchor");
    bool rel = false;
    for (const std::string& l : g_logs) rel = rel || l == "[ab_flip] frame " + std::to_string(mark + 100) + " phase on no 1 rel 100";
    Check(rel, "the phase line carries rel = frame - anchor (100)");
  }
  // anchor + a three phase cycle + order + start offset
  {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin("period=100/anchor=3/start=50/a=wwe2k14_rdr_max_queued=0/b=wwe2k14_rdr_max_queued=1/c=wwe2k14_rdr_max_queued=2/order=a:b:a:c", 1, FakeLog);
    std::vector<uint64_t> at;
    for (uint64_t f = 1; f < 1000; ++f) if (r.AtFrame(f, f >= 400 ? 400 : 0, FakeSet, FakeLog)) at.push_back(f);
    Check(at.size() == 7 && at[1] == 450 && at[2] == 550 && at[6] == 950, "anchor 400 + start 50: flips from frame 450 every 100");
    Check(g_sets[1] == "wwe2k14_rdr_max_queued=0" && g_sets[2] == "wwe2k14_rdr_max_queued=1" && g_sets[3] == "wwe2k14_rdr_max_queued=0" && g_sets[4] == "wwe2k14_rdr_max_queued=2", "order a:b:a:c is followed");
  }
  // first=on: the complementary run starts with the other phase; the reference stays the first written
  {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin(std::string(kGood) + "/first=on", 1, FakeLog);
    std::vector<uint64_t> at;
    for (uint64_t f = 1; f < 700; ++f) if (r.AtFrame(f, 0, FakeSet, FakeLog)) at.push_back(f);
    Check(r.active && at.size() == 3 && g_sets[0] == "wwe2k14_nf_replay_loop=1" && g_sets[2] == "wwe2k14_nf_replay_loop=0", "first=on starts the cycle with phase on");
    Check(g_logs[0].find("cycle on,off") != std::string::npos && g_logs[0].find("reference = off") != std::string::npos, "the start line logs cycle on,off and reference = off");
    Check(Refused(std::string(kGood) + "/first=zzz", "not in the cycle"), "first naming no phase is refused");
  }
  // a rejected setting is reported, not hidden
  {
    Runner r;
    g_sets.clear(); g_logs.clear(); g_reject = true;
    r.Begin(kGood, 1, FakeLog);
    r.AtFrame(2, 0, FakeSet, FakeLog);
    g_reject = false;
    Check(r.set_failures == 2 && g_logs.size() >= 4 && g_logs[2].find("REJECTED") != std::string::npos, "a setting the registry rejects is logged REJECTED");
  }
  // Norm: the registry prints true / false, a spec says 1 / 0
  Check(Norm("true") == "1" && Norm("1") == "1" && Norm("false") == "0" && Norm("0x2") == "2", "Norm: true == 1, false == 0");
  // empty spec: off, nothing logged
  {
    Runner r;
    g_sets.clear(); g_logs.clear();
    r.Begin("", 1, FakeLog);
    for (uint64_t f = 1; f < 100; ++f) r.AtFrame(f, 0, FakeSet, FakeLog);
    Check(!r.active && g_sets.empty() && g_logs.empty(), "an empty spec does nothing and logs nothing");
  }
  (void)kL;
  if (g_fail) {
    std::printf("%d checks FAILED\n", g_fail);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
