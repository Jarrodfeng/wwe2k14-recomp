// Lane 161 (situation director, first verbs): the unit test of src/situation_director_core.h with a fake guest memory and a fake game.
//   director_test.exe            every check must PASS (exit 0)
//   director_test.exe parse      reads lines from stdin, prints the parse of each (the selftest feeds it the strings the resolver and the input script build)
// The planted twins (wrong offset, missing guard, float not in f1, ...) are made by tools/director/director_selftest.py: it edits a COPY of the header and
// requires this test to fail on it.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

#include "situation_director_core.h"

using namespace wwe2k14::situation;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  (ok ? g_pass : g_fail)++;
}

// A fake guest: words in a map; the "game functions" are the same stores the real ones make (read in the listings).
struct Fake {
  std::map<uint32_t, uint32_t> mem;
  Gate gate;
  std::vector<std::string> log;
  std::vector<std::pair<int, uint64_t>> calls;  // every game call: (Fn, r3). B1: the only call a passive makes is SetComActive on the COM component.
  bool team = false;                             // sub_825E7168: the team-shared rules are on
  int stock_cap = 3;
  bool stuck_com = false, stuck_meter = false;  // a game that ignores the store: the read-back must say so
  std::map<uint32_t, uint32_t> list_to_comp;  // sub_82C31CB0: the CHP component of a character's list
  bool stuck_bleed = false;                     // S105 T1: a setter that stores nothing
  bool dev = true;                              // S105 T1: IsDevBuild()
  std::map<uint32_t, uint32_t> list_to_cbdc;    // S105 T1: sub_82C31CB0 with 'CBDC': the liquid component of [char+3736]
  uint32_t U32(uint32_t a) { auto it = mem.find(a); return it == mem.end() ? 0 : it->second; }
  uint8_t U8(uint32_t a) { return uint8_t(U32(a & ~3u) >> (24 - 8 * (a & 3))); }  // big-endian byte lanes of the word map
  void Put8(uint32_t a, uint8_t v) {
    const int sh = 24 - 8 * int(a & 3);
    mem[a & ~3u] = (U32(a & ~3u) & ~(0xFFu << sh)) | (uint32_t(v) << sh);
  }
  void Put32(uint32_t a, uint32_t v) { mem[a] = v; order.push_back("put32 " + std::to_string(a)); }  // big-endian word map: a store is the word
  bool Dev() { return dev; }
  std::vector<uint64_t> bleed_r5;  // every r5 the SetBleed call got, in order
  std::vector<std::vector<uint64_t>> ctor_args;  // S105 SLO3: every DimSceneCtor call's (r3, r4, r5)
  std::vector<uint64_t> alloc_sizes;             // S105 SLO3: every Alloc size
  bool alloc_fails = false;
  std::vector<std::string> order;  // S105 T1: the writes and the call, in the order they happened
  uint32_t Call3(Fn f, uint64_t r3, uint64_t r4, uint64_t r5) {
    calls.push_back({int(f), r3});
    if (f == Fn::DimSceneCtor) { ctor_args.push_back({r3, r4, r5}); mem[uint32_t(r3) + 80] = 0x1234; mem[uint32_t(r3) + 84] = 0x5678; mem[uint32_t(r3) + 88] = 0x9ABC; order.push_back("ctor"); return uint32_t(r3); }
    if (f != Fn::SetBleed) return 0;
    // sub_825A1F28 as read: G+15229 == 0 -> level r30 = -1 (0x825A1F44 / 0x825A1F50), else the r5 given; the level byte at +1392 is level + 1 (clear = 0)
    if (stuck_bleed) return 0;
    bleed_r5.push_back(r5);
    order.push_back(std::string("call opt=") + (U8(0x8376AAF4u + 15229) ? "1" : "0"));
    const int64_t lvl = U8(0x8376AAF4u + 15229) == 0 ? -1 : int64_t(r5);
    Put8(uint32_t(r3) + 1392, uint8_t(lvl + 1));
    return 0;
  }
  float F32(uint32_t a) { uint32_t u = U32(a); float f; std::memcpy(&f, &u, 4); return f; }
  bool Ok(uint32_t p) { return p >= 0x10000; }
  Gate GetGate() { return gate; }
  std::string Clock() { return "42"; }
  void Log(const std::string& s) { log.push_back(s); }
  uint32_t Call(Fn f, uint64_t r3, uint64_t r4, double f1) {
    calls.push_back({int(f), r3});
    switch (f) {
      case Fn::SetComActive: if (!stuck_com) mem[uint32_t(r3) + 100] = uint32_t(r4); return 0;        // stw r4,100(r3)
      case Fn::TeamShared: return team ? 1 : 0;
      case Fn::FindComponent: if (r4 == 0x43424443) return list_to_cbdc.count(uint32_t(r3)) ? list_to_cbdc[uint32_t(r3)] : 0;
        return r4 == 0x43485020 && list_to_comp.count(uint32_t(r3)) ? list_to_comp[uint32_t(r3)] : 0;
      case Fn::SetMeter: {  // stfs f31,76(r31): the meter is whatever arrived in f1 (0 when the caller forgot it)
        if (stuck_meter) return 0;
        float v = float(f1);
        uint32_t u; std::memcpy(&u, &v, 4);
        mem[uint32_t(r3) + 76] = u;
        return 0;
      }
      case Fn::AddStock: { uint32_t ch = mem[uint32_t(r3) + 32]; if (int(mem[ch + 1928]) < stock_cap) mem[ch + 1928]++; return 0; }
      case Fn::RemoveStock: { uint32_t ch = mem[uint32_t(r3) + 32]; if (int(mem[ch + 1928]) > 0) mem[ch + 1928]--; return 0; }
      case Fn::Alloc: alloc_sizes.push_back(r3); return alloc_fails ? 0u : 0x30000000u;  // sub_82250050(size)
      case Fn::SetBleed: case Fn::DimSceneCtor: return 0;
    }
    return 0;
  }
  // slot -> character 0x10000000 + slot * 0x10000; comp = ch + 0x1000; chp comp = ch + 0x2000, its data = ch + 0x3000 (data+32 = the character)
  void Char(int slot, uint32_t role, uint32_t cpu_word, bool with_com = true) {
    // The layout is written as the game's literal numbers (listings), NOT as the core's constants, so a wrong constant in the core is caught.
    const uint32_t ch = 0x10000000u + uint32_t(slot) * 0x10000u;
    mem[0x836F38D0u + 4u * uint32_t(slot)] = ch;
    mem[ch + 1220] = uint32_t(slot);
    mem[ch + 3720] = role;
    if (with_com) { mem[ch + 3088] = ch + 0x1000; mem[ch + 0x1000 + 100] = cpu_word; }
    mem[ch + 3728] = ch + 0x4000;
    list_to_comp[ch + 0x4000] = ch + 0x2000;
    mem[ch + 0x2000 + 24] = ch + 0x3000;
    mem[ch + 0x3000 + 32] = ch;
    mem[ch + 1928] = 1;
    // S105 T1: the liquid component list at char+3736 (NOT 3728): 'CBDC' -> comp (+24 -> X), X+36 = its map (non-zero = the pool is built)
    mem[ch + 3736] = ch + 0x5000;
    list_to_cbdc[ch + 0x5000] = ch + 0x6000;
    mem[ch + 0x6000 + 24] = ch + 0x7000;
    mem[ch + 0x7000 + 36] = 0x20000000u;
    float m = 0.25f; uint32_t u; std::memcpy(&u, &m, 4); mem[ch + 0x3000 + 76] = u;
  }
  uint32_t ChOf(int slot) { return 0x10000000u + uint32_t(slot) * 0x10000u; }
  uint32_t CpuWord(int slot) { return U32(ChOf(slot) + 0x1000 + 100); }
};

static Fake Match() {  // slot 0 human (CPU word 0), slots 1 and 2 CPU, slot 6 the referee, a running match
  Fake f;
  f.gate.seq = 9; f.gate.clock_started = true;
  f.Char(0, 0, 0); f.Char(1, 0, 1); f.Char(2, 0, 1); f.Char(6, 6, 1);
  return f;
}
static std::string Last(Fake& f) { return f.log.empty() ? std::string("(no log line)") : f.log.back(); }
static bool Starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
static std::string Do(Fake& f, State& st, const std::string& text) {
  Cmd c;
  const std::string err = Parse(text, c);
  if (!err.empty()) return err;
  return Run(f, st, c);
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "parse") == 0) {
    for (std::string line; std::getline(std::cin, line);) {
      Cmd c;
      const std::string err = Parse(line, c);
      if (!err.empty()) { std::printf("ERR\n"); continue; }
      std::printf("OK %d %d %d %d %d\n", int(c.verb), c.slot, c.all ? 1 : 0, c.on ? 1 : 0, c.value);
    }
    return 0;
  }

  // ---- the parse rule ----
  {
    Cmd c;
    bool ok = Parse("passive 1 on", c).empty() && c.verb == Verb::Passive && c.slot == 1 && c.on && !c.all;
    ok = ok && Parse("passive all off", c).empty() && c.all && !c.on;
    ok = ok && Parse("momentum 2 100", c).empty() && c.verb == Verb::Momentum && c.slot == 2 && c.value == 100;
    ok = ok && Parse("stock 0 3", c).empty() && c.verb == Verb::Stock && c.value == 3;
    ok = ok && Parse("passive_cpu on", c).empty() && c.verb == Verb::PassiveCpu && c.on;
    Check(ok, "parse.good", "every verb's good form");
    bool bad = true;
    for (const char* t : {"", "passive", "passive 9 on", "passive x on", "passive 1", "passive 1 maybe", "passive 1 on extra", "momentum 1", "momentum 1 101", "momentum 1 -5",
                          "momentum 1 5x", "stock 1 10", "stock 1 -1", "stock 12 1", "passive_cpu", "passive_cpu 1", "dance 1 2"})
      bad = bad && !Parse(t, c).empty();
    Check(bad, "parse.bad", "17 bad forms are all errors");
  }

  // ---- the match guard ----
  {
    Gate g; g.seq = 9; g.clock_started = true;
    Check(GateVerdict(g).empty(), "gate.running", "");
    Gate a = g; a.seq = 75; Check(Starts(GateVerdict(a), "busy: no match"), "gate.not_in_game");
    a = g; a.clock_started = false; Check(Starts(GateVerdict(a), "busy: the bell"), "gate.no_bell");
    a = g; a.g472 = 1; Check(Starts(GateVerdict(a), "refused: the match is already decided"), "gate.decided");
    a = g; a.g468 = 1; Check(Starts(GateVerdict(a), "busy: a scripted scene"), "gate.scene");
    Fake f = Match(); State st;
    f.gate.seq = 75;
    const std::string r = Do(f, st, "passive 1 on");
    Check(Starts(r, "busy: no match") && f.CpuWord(1) == 1 && f.calls.empty(), "run.refuses_outside_match", r);
    f.gate.seq = 9; f.gate.g472 = 3;
    Check(Starts(Do(f, st, "momentum 1 100"), "refused") && f.F32(f.ChOf(1) + 0x3000 + 76) == 0.25f, "run.refuses_decided_match");
  }

  // ---- passive ----
  {
    Fake f = Match(); State st;
    std::string r = Do(f, st, "passive 1 on");
    Check(Starts(r, "ok") && f.CpuWord(1) == 0, "passive.on", r);
    Check(f.calls.size() == 1 && f.calls[0].first == int(Fn::SetComActive) && f.calls[0].second == f.ChOf(1) + 0x1000 && f.mem.count(f.ChOf(1) + 1888) == 0 && f.U32(f.ChOf(1) + 1928) == 1,
          "passive.makes_exactly_one_call_on_the_component", "B1: no pad-record call, the character's own words are untouched");
    Check(f.CpuWord(2) == 1 && f.CpuWord(0) == 0, "passive.only_that_slot");
    Check(!f.log.empty() && Last(f).find("was 1, now 0") != std::string::npos, "passive.logs_readback", f.log.empty() ? "" : Last(f));
    r = Do(f, st, "passive 1 off");
    Check(Starts(r, "ok") && f.CpuWord(1) == 1, "passive.off_restores_the_original_word", r);
    r = Do(f, st, "passive 1 off");
    Check(Starts(r, "refused"), "passive.off_twice_refused", r);
    r = Do(f, st, "passive 0 off");
    Check(Starts(r, "refused") && f.CpuWord(0) == 0, "passive.off_on_a_human_refused", r);
    r = Do(f, st, "passive 6 on");
    Check(Starts(r, "refused") && Starts(r, "refused: slot 6 is not a wrestler"), "passive.referee_refused", r);
    r = Do(f, st, "passive 4 on");
    Check(Starts(r, "refused: slot 4 holds no character"), "passive.empty_slot_refused", r);
    f.mem[f.ChOf(2) + 1220] = 7;  // a character that is not that slot's
    r = Do(f, st, "passive 2 on");
    Check(Starts(r, "refused") && f.CpuWord(2) == 1, "passive.slot_mismatch_refused", r);
    f.mem[f.ChOf(2) + 1220] = 2;
    f.mem[f.ChOf(2) + 3088] = 0;
    r = Do(f, st, "passive 2 on");
    Check(Starts(r, "refused: slot 2 has no COM component"), "passive.no_component_refused", r);
  }
  {
    Fake f = Match(); State st;
    f.stuck_com = true;
    std::string r = Do(f, st, "passive 1 on");
    Check(Starts(r, "failed") && r.find("read-back") != std::string::npos, "passive.stuck_store_is_a_failure", r);
  }
  // all
  {
    Fake f = Match(); State st;
    std::string r = Do(f, st, "passive all on");
    Check(Starts(r, "ok") && f.CpuWord(1) == 0 && f.CpuWord(2) == 0 && f.CpuWord(6) == 1 && f.calls.size() == 2, "passive_all.on", r);
    Check(r.find("skipped 0") != std::string::npos, "passive_all.reports_the_human_skipped", r);
    r = Do(f, st, "passive all off");
    Check(Starts(r, "ok") && f.CpuWord(1) == 1 && f.CpuWord(2) == 1 && f.CpuWord(0) == 0, "passive_all.off_restores", r);
    Fake g = Match(); State s2;
    g.mem[g.ChOf(1) + 0x1000 + 100] = 0; g.mem[g.ChOf(2) + 0x1000 + 100] = 0;
    r = Do(g, s2, "passive all on");
    Check(Starts(r, "failed: passive all on: no CPU-driven"), "passive_all.none_to_do_is_a_failure", r);
  }
  // S2 / S1 / S5
  {
    Fake f = Match(); State st;
    std::string r = Do(f, st, "passive 0 on");
    Check(Starts(r, "refused: slot 0 is not CPU-driven") && f.calls.empty(), "passive.on_a_human_refused", r);
    Do(f, st, "passive 1 on");
    f.mem[f.ChOf(1) + 0x1000 + 100] = 1;  // the game (a script's verb 513) gave the wrestler back
    f.calls.clear();
    r = Do(f, st, "passive 1 off");
    Check(Starts(r, "refused") && r.find("already 1") != std::string::npos && f.calls.empty() && !st.s[1].on, "passive.off_when_the_word_changed_writes_nothing_and_drops_ownership", r);
    Fake g = Match(); State s2;
    Do(g, s2, "passive 1 on");
    g.gate.g472 = 2; g.gate.g468 = 1;  // decided, and a scene
    r = Do(g, s2, "passive 1 off");
    Check(Starts(r, "ok") && g.CpuWord(1) == 1, "passive.off_allowed_after_the_decision_and_in_a_scene", r);
    Do(g, s2, "passive 2 on");
    r = Do(g, s2, "momentum 1 50");
    Check(Starts(r, "refused: the match is already decided"), "momentum.still_refused_after_the_decision", r);
    r = Do(g, s2, "passive 2 on");
    Check(Starts(r, "refused"), "passive.on_still_refused_after_the_decision", r);
    g.gate.g472 = 0; g.gate.g468 = 0;
    Do(g, s2, "passive 2 on");
    g.gate.clock_started = false;  // before a bell (a new match): off needs only the program
    r = Do(g, s2, "passive 2 off");
    Check(Starts(r, "ok"), "passive.off_needs_only_seq_9", r);
    g.gate.seq = 75;
    r = Do(g, s2, "passive all off");
    Check(Starts(r, "busy"), "passive.off_outside_a_match_program_is_busy", r);
    Check(IsOffForm(Cmd{Verb::Passive, 1, false, false, 0}) && !IsOffForm(Cmd{Verb::Passive, 1, false, true, 0}) && IsOffForm(Cmd{Verb::PassiveCpu, -1, false, false, 0}) &&
              !IsOffForm(Cmd{Verb::Momentum, 1, false, false, 5}), "replay.off_forms", "only the give-back forms pass while a recording plays");
    // S1: leaving the in-game sequence forgets the remembered words
    Fake h = Match(); State s3; s3.passive_cpu = true;
    Do(h, s3, "passive 1 on");
    Gate away; away.seq = 75;
    Housekeeping(s3, away);
    Check(!s3.s[1].on && s3.passive_cpu, "state.forgotten_outside_the_match_but_the_latch_stays", "");
    Gate in; in.seq = 9; in.clock_started = true;
    Housekeeping(s3, in);
    Check(s3.passive_cpu, "state.housekeeping_in_game_keeps_the_latch", "");
    h.mem[h.ChOf(1) + 0x1000 + 100] = 1;
    Check(Starts(Do(h, s3, "passive 1 on"), "ok") && s3.s[1].on, "state.claim_made_before_the_reset", "");
    in.clock_started = false;  // F5: a new match before its bell
    Housekeeping(s3, in);
    Check(!s3.s[1].on && s3.passive_cpu, "state.forgotten_when_the_clock_is_back_at_0", "");
    in.clock_started = true;
    h.mem[h.ChOf(1) + 0x1000 + 100] = 1;  // the new match's wrestler is CPU-driven again
    Do(h, s3, "passive 1 on");
    Housekeeping(s3, in);
    Check(s3.s[1].on, "state.kept_in_a_running_match", "");
    // F4: a waiting script verb dies with the match it saw
    Check(!DropsRetry(false, 75) && !DropsRetry(false, 9) && !DropsRetry(true, 9) && DropsRetry(true, 75) && DropsRetry(true, 4), "retry.dropped_when_the_match_it_saw_is_over", "");
  }
  // S4: team-shared rules
  {
    Fake f = Match(); State st;
    f.team = true;
    std::string r = Do(f, st, "momentum 1 100");
    Check(Starts(r, "refused: team-shared") && f.F32(f.ChOf(1) + 0x3000 + 76) == 0.25f, "momentum.refused_in_team_shared_rules", r);
    r = Do(f, st, "stock 1 3");
    Check(Starts(r, "refused: team-shared") && f.U32(f.ChOf(1) + 1928) == 1, "stock.refused_in_team_shared_rules", r);
  }
  // passive_cpu latch
  {
    Fake f = Match(); State st;
    f.gate.seq = 75;
    std::string r = Do(f, st, "passive_cpu on");
    Check(Starts(r, "ok") && st.passive_cpu, "passive_cpu.sets_the_latch_with_no_match", r);
    AutoState a;
    Gate g; g.seq = 9; g.clock_started = false;
    bool due1 = AutoDue(a, true, g);
    g.clock_started = true;
    bool due2 = AutoDue(a, true, g), due3 = AutoDue(a, true, g);
    g.g468 = 1; a.applied = false; bool due4 = AutoDue(a, true, g);
    g.g468 = 0; bool due5 = AutoDue(a, false, g);
    g.seq = 75; AutoDue(a, true, g); g.seq = 9; bool due6 = AutoDue(a, true, g);
    Check(!due1 && due2 && !due3 && !due4 && !due5 && due6, "passive_cpu.once_per_match", "before the bell no, at the bell yes, then not again, not in a scene, off no, a new match yes");
  }

  // ---- momentum ----
  {
    Fake f = Match(); State st;
    std::string r = Do(f, st, "momentum 1 100");
    Check(Starts(r, "ok") && f.F32(f.ChOf(1) + 0x3000 + 76) == 1.0f, "momentum.full", r);
    r = Do(f, st, "momentum 2 40");
    const float v = f.F32(f.ChOf(2) + 0x3000 + 76);
    Check(Starts(r, "ok") && std::fabs(v - 0.4f) < 1e-6f && f.F32(f.ChOf(1) + 0x3000 + 76) == 1.0f, "momentum.percent_to_fraction", r);
    Check(Last(f).find("meter was 0.250, now 0.400") != std::string::npos, "momentum.logs_before_and_after", Last(f));
    r = Do(f, st, "momentum 6 50");
    Check(Starts(r, "refused"), "momentum.referee_refused", r);
    r = Do(f, st, "momentum 5 50");
    Check(Starts(r, "refused"), "momentum.empty_slot_refused", r);
    f.list_to_comp.clear();
    r = Do(f, st, "momentum 1 10");
    Check(Starts(r, "refused: the character has no health component"), "momentum.no_chp_refused", r);
  }
  {
    Fake f = Match(); State st;
    f.stuck_meter = true;
    std::string r = Do(f, st, "momentum 1 100");
    Check(Starts(r, "failed") && r.find("read-back") != std::string::npos, "momentum.stuck_store_is_a_failure", r);
  }
  // ---- stock ----
  {
    Fake f = Match(); State st;
    std::string r = Do(f, st, "stock 1 3");
    Check(Starts(r, "ok") && f.U32(f.ChOf(1) + 1928) == 3, "stock.add_to_three", r);
    r = Do(f, st, "stock 1 0");
    Check(Starts(r, "ok") && f.U32(f.ChOf(1) + 1928) == 0, "stock.remove_to_zero", r);
    r = Do(f, st, "stock 2 5");
    Check(Starts(r, "failed") && f.U32(f.ChOf(2) + 1928) == 3, "stock.cap_is_a_failure_with_the_number", r);
    Check(f.U32(f.ChOf(1) + 1928) == 0, "stock.other_slot_untouched");
    r = Do(f, st, "stock 6 1");
    Check(Starts(r, "refused"), "stock.referee_refused", r);
  }

  // ---- bleed (S105 T1: dev only; the option first, then the level; read-back) ----
  {
    Cmd c;
    // the parser: slot 0-8, level 1-3, one token each
    bool ok = Parse("bleed 0 3", c).empty() && c.verb == Verb::Bleed && c.slot == 0 && c.value == 3;
    ok = ok && Parse("bleed 8 1", c).empty() && c.slot == 8 && c.value == 1;
    Check(ok && IsVerb("bleed"), "bleed.parse_good", "bleed 0 3 / bleed 8 1");
    const char* bad[] = {"bleed", "bleed 0", "bleed 9 1", "bleed 0 0", "bleed 0 4", "bleed 0 12", "bleed x 1", "bleed 0 3 3", "bleed -1 1", "bleed 0 -1"};
    bool all_bad = true;
    for (const char* b : bad) all_bad = all_bad && !Parse(b, c).empty();
    Check(all_bad, "bleed.parse_bad", "slot 9, level 0 / 4 / 12, malformed, extra words");
  }
  {
    Fake f = Match(); State st;  // the order: the option is written before the level call (the setter forces -1 while the option is 0)
    const uint32_t g = 0x8376AAF4u + 15229, ch = f.ChOf(0);
    Check(f.U8(g) == 0, "bleed.fake_starts_blood_off");
    std::string r = Do(f, st, "bleed 0 3");
    Check(Starts(r, "ok") && f.U8(g) == 1 && f.U8(ch + 1398) == 1 && f.U8(ch + 1392) == 3, "bleed.ok_path", r);
    Check(f.bleed_r5.size() == 1 && f.bleed_r5[0] == 2, "bleed.level_minus_one_in_r5", f.bleed_r5.empty() ? "no call" : std::to_string(f.bleed_r5[0]));
    Check(f.order.size() == 1 && f.order[0] == "call opt=1", "bleed.option_set_before_the_call", f.order.empty() ? "no call" : f.order[0]);
    bool turned = false, line = false;
    for (const std::string& l : f.log) {
      if (l.find("turned it on") != std::string::npos) turned = true;
      if (l.find("bleed slot 0 level 3: ok") != std::string::npos) line = true;
    }
    Check(turned && line, "bleed.logs_the_option_and_the_verdict");
    r = Do(f, st, "bleed 1 1");  // option already 1: not written again, level 1 -> r5 = 0
    Check(Starts(r, "ok") && f.U8(f.ChOf(1) + 1392) == 1 && f.bleed_r5.size() == 2 && f.bleed_r5[1] == 0, "bleed.level_one_is_r5_zero", r);
  }
  {
    Fake f = Match(); State st;  // F3: lowering the level is refused (the setter never clears the higher set)
    std::string r = Do(f, st, "bleed 0 3");
    r = Do(f, st, "bleed 0 1");
    Check(Starts(r, "refused") && f.U8(f.ChOf(0) + 1392) == 3 && f.bleed_r5.size() == 1, "bleed.lowering_is_refused_level_kept", r);
    r = Do(f, st, "bleed 0 3");
    Check(Starts(r, "ok"), "bleed.same_level_again_is_ok", r);
  }
  {
    Fake f = Match(); State st;  // the dev gate: a player build never runs it, whatever else is true
    f.dev = false;
    std::string r = Do(f, st, "bleed 0 3");
    Check(Starts(r, "refused") && f.calls.empty() && f.U8(0x8376AAF4u + 15229) == 0 && f.U8(f.ChOf(0) + 1398) == 0, "bleed.player_build_refused_no_writes", r);
    f.gate.seq = 0;  // not even a busy / match answer in a player build
    r = Do(f, st, "bleed 0 3");
    Check(Starts(r, "refused: bleed is a dev-build verb"), "bleed.player_build_refused_before_the_match_gate", r);
  }
  {
    Fake f = Match(); State st;  // busy: no pool yet (every link of [char+3736] -> 'CBDC' -> +24 -> +36), and nothing written
    f.mem[f.ChOf(0) + 0x7000 + 36] = 0;
    std::string r = Do(f, st, "bleed 0 2");
    Check(Starts(r, "busy") && f.calls.size() == 1 && f.U8(0x8376AAF4u + 15229) == 0 && f.U8(f.ChOf(0) + 1398) == 0, "bleed.no_map_is_busy_and_writes_nothing", r);
    f.mem[f.ChOf(0) + 0x7000 + 36] = 0x20000000u;
    f.mem[f.ChOf(0) + 0x6000 + 24] = 0;
    Check(Starts(Do(f, st, "bleed 0 2"), "busy"), "bleed.no_liquid_object_is_busy");
    f.list_to_cbdc.clear();
    Check(Starts(Do(f, st, "bleed 0 2"), "busy"), "bleed.no_cbdc_component_is_busy");
    f.mem[f.ChOf(0) + 3736] = 0;
    Check(Starts(Do(f, st, "bleed 0 2"), "busy"), "bleed.no_list_is_busy");
    Fake h = Match();
    h.gate.clock_started = false;
    Check(Starts(Do(h, st, "bleed 0 2"), "busy"), "bleed.before_the_bell_is_busy");
  }
  {
    Fake f = Match(); State st;
    Check(Starts(Do(f, st, "bleed 6 2"), "refused"), "bleed.referee_refused");
    Check(Starts(Do(f, st, "bleed 5 2"), "refused"), "bleed.empty_slot_refused");
  }
  {
    Fake f = Match(); State st;  // a game that ignores the setter: the read-back says so, with the number
    f.stuck_bleed = true;
    std::string r = Do(f, st, "bleed 2 3");
    Check(Starts(r, "failed") && r.find("readback 0") != std::string::npos, "bleed.stuck_setter_is_a_failure", r);
    Check(Last(f).find("readback 0") != std::string::npos, "bleed.readback_is_logged", Last(f));
  }
  // ---- lightsout (S105 SLO3: dev only; Alloc(136) then ctor(obj, attacker, target); busy while the brightness is not 1.0) ----
  {
    Cmd c;
    bool ok = Parse("lightsout 0", c).empty() && c.verb == Verb::LightsOut && c.slot == 0;
    ok = ok && Parse("lightsout 8", c).empty() && c.slot == 8 && IsVerb("lightsout");
    Check(ok, "lightsout.parse_good", "lightsout 0 / lightsout 8");
    bool all_bad = true;
    for (const char* t : {"lightsout", "lightsout 9", "lightsout x", "lightsout 0 1", "lightsout -1", "lightsout 0 on"})
      if (Parse(t, c).empty()) all_bad = false;
    Check(all_bad, "lightsout.parse_bad", "no slot, slot 9, malformed, extra words");
  }
  auto Dim = []() {  // a running match with the dim scene's preconditions: slot 0 targets slot 1, the scene root and the light manager (brightness 1.0)
    Fake f = Match();
    f.mem[0x10000000u + 2672] = 0x10010000u;
    f.mem[0x837631CCu] = 0x20001000u;
    f.mem[0x20001000u] = 0x20002000u;
    f.mem[0x83924778u] = 0x21000000u;
    f.mem[0x21000000u + 452] = 0x3F800000u;
    return f;
  };
  {
    Fake f = Dim(); State st;
    std::string r = Do(f, st, "lightsout 0");
    Check(Starts(r, "ok"), "lightsout.ok_path", r);
    Check(f.alloc_sizes.size() == 1 && f.alloc_sizes[0] == 136, "lightsout.alloc_136", f.alloc_sizes.empty() ? "no alloc" : std::to_string(f.alloc_sizes[0]));
    Check(f.ctor_args.size() == 1 && f.ctor_args[0][0] == 0x30000000u && f.ctor_args[0][1] == f.ChOf(0) && f.ctor_args[0][2] == 0x10010000u, "lightsout.ctor_obj_attacker_target_in_order",
          f.ctor_args.empty() ? "no ctor" : std::to_string(f.ctor_args[0][0]) + " " + std::to_string(f.ctor_args[0][1]) + " " + std::to_string(f.ctor_args[0][2]));
    Check(f.calls.size() == 2 && f.calls[0].first == int(Fn::Alloc) && f.calls[1].first == int(Fn::DimSceneCtor), "lightsout.alloc_before_ctor");
    Check(Last(f).find("lightsout slot 0: ok") != std::string::npos, "lightsout.logs_ok", Last(f));
    // S105 LO2 (run 127): the ctor leaves a trigger frame in obj+80 / +84 (a passive act has none): both are zeroed AFTER the ctor, word-sized, at +80 / +84; +88 untouched
    Check(f.U32(0x30000000u + 80) == 0 && f.U32(0x30000000u + 84) == 0 && f.U32(0x30000000u + 88) == 0x9ABC, "lightsout.trigger_frame_zeroed_at_80_84");
    Check(f.order.size() == 3 && f.order[0] == "ctor" && f.order[1] == "put32 " + std::to_string(0x30000000u + 80) && f.order[2] == "put32 " + std::to_string(0x30000000u + 84), "lightsout.stores_after_ctor_at_80_84");
    {  // twin: the run-127 behaviour (no stores) leaves the ctor's non-zero trigger, so the scene would stay in phase 0
      Fake tw = Dim();
      tw.Call3(Fn::DimSceneCtor, 0x30000000u, 1, 2);
      Check(tw.U32(0x30000000u + 80) != 0 && tw.U32(0x30000000u + 84) != 0, "lightsout.twin_without_stores_keeps_the_trigger");
    }
  }
  {
    Fake f = Dim(); State st; f.dev = false;  // a player build never runs it, not even a busy answer
    std::string r = Do(f, st, "lightsout 0");
    Check(Starts(r, "refused: lightsout is a dev-build verb") && f.calls.empty(), "lightsout.player_build_refused_no_calls", r);
    f.gate.clock_started = false;
    Check(Starts(Do(f, st, "lightsout 0"), "refused: lightsout is a dev-build verb"), "lightsout.player_build_refused_before_the_match_gate");
  }
  {
    Fake f = Dim(); State st;  // the busy rule: a dim already holds the brightness
    f.mem[0x21000000u + 452] = 0x3F000000u;
    std::string r = Do(f, st, "lightsout 0");
    Check(Starts(r, "busy") && f.calls.empty() && Last(f).find("lightsout slot 0: busy") != std::string::npos, "lightsout.dim_running_is_busy_no_calls", r);
    f.mem[0x21000000u + 452] = 0;  // a fully dark arena is also busy
    Check(Starts(Do(f, st, "lightsout 0"), "busy") && f.calls.empty(), "lightsout.fully_dark_is_busy");
  }
  {
    Fake f = Dim(); State st;
    f.mem[0x10000000u + 2672] = 0;
    Check(Starts(Do(f, st, "lightsout 0"), "refused") && f.calls.empty(), "lightsout.no_target_refused");
    Fake g = Dim(); g.mem[0x837631CCu] = 0;
    Check(Starts(Do(g, st, "lightsout 0"), "refused") && g.calls.empty(), "lightsout.no_scene_root_refused");
    Fake h = Dim(); h.mem[0x20001000u] = 0;
    Check(Starts(Do(h, st, "lightsout 0"), "refused") && h.calls.empty(), "lightsout.empty_inner_scene_root_refused");
    Fake m = Dim(); m.mem[0x83924778u] = 0;
    Check(Starts(Do(m, st, "lightsout 0"), "refused") && m.calls.empty(), "lightsout.no_light_manager_refused");
    Fake e2 = Dim();
    Check(Starts(Do(e2, st, "lightsout 5"), "refused") && e2.calls.empty(), "lightsout.empty_slot_refused");
    Check(Starts(Do(e2, st, "lightsout 6"), "refused") && e2.calls.empty(), "lightsout.referee_refused");
    Fake b = Dim(); b.gate.clock_started = false;
    Check(Starts(Do(b, st, "lightsout 0"), "busy") && b.calls.empty(), "lightsout.before_the_bell_is_busy");
    Fake a = Dim(); a.alloc_fails = true;
    std::string r = Do(a, st, "lightsout 0");
    Check(Starts(r, "failed") && a.ctor_args.empty(), "lightsout.alloc_failure_is_a_failure_no_ctor", r);
  }
  std::printf("SUMMARY %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
