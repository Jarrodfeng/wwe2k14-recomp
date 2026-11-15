// S102 S0-4: the switch table on a fake settings store. clang++ -std=c++17 -I src/renderer_adapter src/renderer_adapter/rdr_switches_test.cpp
// Arguments: "plant" = the driver forgets to bump the fake registry's change counter after the flip (the gate's twin: must FAIL).
//            "bench" = time the refresh on the fake store (the number in SWITCHES.md is from the real registry, see there).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "rdr_switches.h"

static std::map<std::string, std::string> store;
static std::vector<std::string> lines;
static uint64_t counter = 1;  // the fake registry's change counter (rex::cvar::ChangeCount)
static uint64_t probes = 0;
static bool ReadStore(const char* name, std::string& text, bool probe) {
  probes += probe;
  auto it = store.find(name);
  if (it == store.end()) return false;
  text = it->second;
  return true;
}
static void LogStore(const char* line) { lines.push_back(line); }
static void Write(const char* name, const char* value, bool bump) {
  store[name] = value;
  if (bump) ++counter;
}
static int Refresh(uint64_t frame) { return rdr_sw::Refresh(frame, ReadStore, LogStore, counter); }

static int fails = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}
static size_t Count(const char* needle) {
  size_t n = 0;
  for (const auto& l : lines)
    if (l.find(needle) != std::string::npos) ++n;
  return n;
}

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "plant") == 0;
  const bool bench = argc > 1 && std::strcmp(argv[1], "bench") == 0;
  for (int i = 0; i < int(rdr_sw::kCount); ++i) store[rdr_sw::Infos()[i].name] = rdr_sw::Infos()[i].def ? "true" : "false";
  store["wwe2k14_rdr_max_queued"] = "2";
  store.erase("wwe2k14_rdr_copy_alias");

  Check(rdr_sw::IntOr(rdr_sw::kMaxQueued, 7) == 7 && rdr_sw::BoolSeen(rdr_sw::kSkipPrune, false, 0) == false, "before the first refresh a reader gets its fallback (and acks nothing)");
  Refresh(1);
  Check(Count("[switch] table live at frame 1") == 1 && Count("1 not in this build") == 1, "the first refresh logs one summary and names the missing setting");
  Check(Count("not a known setting") == 1, "the missing setting is named once");
  Check(rdr_sw::IntOr(rdr_sw::kMaxQueued, 7) == 2 && rdr_sw::Bool(rdr_sw::kSkipPrune) && !rdr_sw::Bool(rdr_sw::kSkipKeepsState), "values come from the settings");
  const uint32_t gen0 = rdr_sw::g.generation.load();
  const size_t lines0 = lines.size();

  // MUST A: no registry change, no read at all.
  const uint64_t reads0 = rdr_sw::g.reads, probes0 = probes;
  for (int f = 2; f < 12; ++f) Refresh(f);
  Check(rdr_sw::g.reads == reads0 && lines.size() == lines0 && rdr_sw::g.generation.load() == gen0, "ten frames with the counter unmoved: 0 reads, no line, no generation bump");
  Write("wwe2k14_rdr_skip_prune", "false", false);   // a write the counter never heard of
  Refresh(20);
  Check(rdr_sw::Bool(rdr_sw::kSkipPrune) && rdr_sw::g.reads == reads0, "a change WITHOUT a counter bump does not reach the struct (and costs no read)");
  if (!plant) ++counter;                              // the counter moves (the planted run forgets it)
  const uint64_t before = rdr_sw::g.reads;
  Refresh(101);
  Check(!rdr_sw::Bool(rdr_sw::kSkipPrune), "a change WITH a bump reaches the struct at the next frame");
  Check(rdr_sw::g.reads - before == uint64_t(int(rdr_sw::kCount) - 1) && probes == probes0, "that refresh reads each known setting once and probes none (no second GetFlagInfo)");
  Check(rdr_sw::g.generation.load() == gen0 + 1, "the generation counter moved by one");
  Check(Count("[switch] frame 101 wwe2k14_rdr_skip_prune=false") == 1, "one '[switch] frame N NAME=VALUE' line");
  ++counter;
  Refresh(102);
  ++counter;
  Refresh(103);
  Check(Count("wwe2k14_rdr_skip_prune=false") == 1, "it logs once, not every frame");

  // Review C: the reader's acknowledgement, once per new generation.
  Check(rdr_sw::BoolSeen(rdr_sw::kSkipPrune, true, 105) == false && rdr_sw::BoolSeen(rdr_sw::kSkipPrune, true, 106) == false, "the reader sees the new value");
  Check(Count("[switch] seen frame 105 gen 1 wwe2k14_rdr_skip_prune=0") == 1 && Count("[switch] seen") == 1, "it acknowledges once, on the first frame it reads the new generation");
  Check(rdr_sw::BoolSeen(rdr_sw::kSkipKeepsState, false, 107) == false && Count("[switch] seen") == 1, "a switch that never changed acknowledges nothing");

  // Flip back and an int switch.
  Write("wwe2k14_rdr_skip_prune", "true", true);
  Write("wwe2k14_rdr_max_queued", "4", false);
  Refresh(200);
  Check(rdr_sw::Bool(rdr_sw::kSkipPrune) && rdr_sw::Int(rdr_sw::kMaxQueued) == 4 && rdr_sw::g.generation.load() == gen0 + 3, "two switches, one counter step: both read (a refresh reads every known setting)");

  // A start-only switch is refused, once per value.
  Write("wwe2k14_rdr_crowd_ours", "true", true);
  Refresh(300);
  Write("wwe2k14_rdr_crowd_ours", "true", true);
  Refresh(301);
  Check(!rdr_sw::Bool(rdr_sw::kCrowdOurs), "a start-only switch keeps its start value");
  Check(Count("refused frame 300 wwe2k14_rdr_crowd_ours=true start-only") == 1 && Count("refused") == 1, "the refusal is one log line");

  rdr_sw::NoteWrite("wwe2k14_something_else", "5", 400, LogStore);
  rdr_sw::NoteWrite("wwe2k14_rdr_skip_prune", "true", 400, LogStore);
  Check(Count("[switch] frame 400 wwe2k14_something_else=5 (not in the table)") == 1 && Count("frame 400") == 1, "an untabled setting is logged by NoteWrite; a tabled one is left to the refresh");

  // S102 T1: lazy banks is a per-frame switch: flipped mid-run, the live reader sees it on the next frame and acknowledges it once.
  Check(rdr_sw::Infos()[rdr_sw::kLazyBanks].kind == rdr_sw::Kind::kPerFrame && !rdr_sw::BoolSeen(rdr_sw::kLazyBanks, true, 500) && Count("wwe2k14_rdr_lazy_banks") == 0,
        "lazy_banks: per-frame, default off, nothing acknowledged before a change");
  Write("wwe2k14_rdr_lazy_banks", "true", true);
  Refresh(510);
  Check(Count("[switch] frame 510 wwe2k14_rdr_lazy_banks=true") == 1, "lazy_banks flipped mid-run: one switch line");
  Check(rdr_sw::BoolSeen(rdr_sw::kLazyBanks, false, 511) && rdr_sw::BoolSeen(rdr_sw::kLazyBanks, false, 512) &&
            Count("[switch] seen frame 511 gen ") == 1 && Count("wwe2k14_rdr_lazy_banks=1") == 1,
        "lazy_banks: the reader sees true on the next frame and logs the 'seen' line once");

  if (bench) {
    const auto t0 = std::chrono::steady_clock::now();
    const int n = 100000;
    for (int i = 0; i < n; ++i) {
      ++counter;
      Refresh(1000 + i);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("bench: a refresh that reads all %d settings from the fake store: %.2f us; with the counter unmoved: ", int(rdr_sw::kCount) - 1, us);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) Refresh(2000000 + i);
    std::printf("%.4f us\n", std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / n);
  }
  std::printf("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
