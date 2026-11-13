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
  uint32_t U32(uint32_t a) { auto it = mem.find(a); return it == mem.end() ? 0 : it->second; }
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
      case Fn::FindComponent: return r4 == 0x43485020 && list_to_comp.count(uint32_t(r3)) ? list_to_comp[uint32_t(r3)] : 0;
      case Fn::SetMeter: {  // stfs f31,76(r31): the meter is whatever arrived in f1 (0 when the caller forgot it)
        if (stuck_meter) return 0;
        float v = float(f1);
        uint32_t u; std::memcpy(&u, &v, 4);
        mem[uint32_t(r3) + 76] = u;
        return 0;
      }
      case Fn::AddStock: { uint32_t ch = mem[uint32_t(r3) + 32]; if (int(mem[ch + 1928]) < stock_cap) mem[ch + 1928]++; return 0; }
      case Fn::RemoveStock: { uint32_t ch = mem[uint32_t(r3) + 32]; if (int(mem[ch + 1928]) > 0) mem[ch + 1928]--; return 0; }
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

  std::printf("SUMMARY %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
