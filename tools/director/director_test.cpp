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
  std::vector<std::vector<uint64_t>> state_calls;  // S110 OMGROUTE: every QueueState call's (r3, r4, r5)
  std::vector<std::string> order;  // S105 T1: the writes and the call, in the order they happened
  struct WishCall { int slot; uint32_t ch; int wish; };
  std::vector<WishCall> wishes;  // S110 FINPICK: every FinisherWish call (the glue stores the wish for the hook)
  std::string FinisherWish(int slot, uint32_t ch, int wish) { wishes.push_back({slot, ch, wish}); return "fake fit"; }
  struct OmgCall { int slot; uint32_t ch; bool on; };
  std::vector<OmgCall> omgs;  // S110 OMGVERB: every OmgWish call (the glue reads the gates and arms the hook)
  std::string OmgWish(int slot, uint32_t ch, bool on) { omgs.push_back({slot, ch, on}); return "fake gates"; }
  uint32_t Call3(Fn f, uint64_t r3, uint64_t r4, uint64_t r5) {
    calls.push_back({int(f), r3});
    if (f == Fn::DimSceneCtor) { ctor_args.push_back({r3, r4, r5}); mem[uint32_t(r3) + 80] = 0x1234; mem[uint32_t(r3) + 84] = 0x5678; mem[uint32_t(r3) + 88] = 0x9ABC; order.push_back("ctor"); return uint32_t(r3); }
    if (f == Fn::QueueState) { state_calls.push_back({r3, r4, r5}); return 1; }
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
      case Fn::PlaceOf: pcalls.push_back("place"); return F32(uint32_t(r3)) > 0 ? 7 : 2;  // S110 PLACE: sub_82612E88 reads the x stored at r3 = char+160 (the code depends on it)
      case Fn::SetBleed: case Fn::DimSceneCtor: case Fn::QueueState: case Fn::SetYaw: case Fn::FloorY: case Fn::Warp: return 0;
    }
    return 0;
  }
  // S110 PLACE: the game's functions as read (sub_822FB578 stores +148; sub_82612BF8 reads the place byte at +70; sub_829C0EE8 writes the mirrors and its OWN +70 rule)
  std::vector<std::string> pcalls;  // the place verb's game calls in order
  std::vector<double> pargs;        // their float arguments in order (yaw radians; then x, y, z)
  static uint32_t Bits(double d) { float v = float(d); uint32_t u; std::memcpy(&u, &v, 4); return u; }
  uint32_t CallF(Fn f, uint64_t r3, double f1) {
    calls.push_back({int(f), r3});
    pcalls.push_back("yaw"); pargs.push_back(f1);
    mem[uint32_t(r3) + 148] = Bits(f1);
    return 0;
  }
  double CallRetF(Fn f, uint64_t r3, uint64_t r4) {
    calls.push_back({int(f), r3});
    pcalls.push_back("floor");
    if (r4 != 255) return 12345.0;
    const uint8_t p = U8(uint32_t(r3) + 70);  // the floor height depends on the place byte already stored
    return p == 7 ? -120.0 : (p == 2 ? -605.0 : 999.0);
  }
  uint32_t CallF3(Fn f, uint64_t r3, double f1, double f2, double f3) {
    calls.push_back({int(f), r3});
    pcalls.push_back("warp"); pargs.push_back(f1); pargs.push_back(f2); pargs.push_back(f3);
    const uint32_t ch = uint32_t(r3), ctrl = U32(ch + 3096);
    mem[ch + 160] = Bits(f1); mem[ch + 164] = Bits(f2); mem[ch + 168] = Bits(f3);
    mem[ch + 192] = Bits(f1); mem[ch + 200] = Bits(f3);
    if (ctrl) { mem[ctrl + 48] = Bits(f1); mem[ctrl + 56] = Bits(f3); }  // not ctrl+32: the warp writes no angle
    Put8(ch + 70, 9);  // its own story-scene place rule: not the match's code
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
  // ---- state (S110 OMGROUTE: dev only; the wrestler's own registry [[char+3148]+40]: count, records of {ctor, name ptr}; one call with the record's own name pointer) ----
  {
    Cmd c;
    bool ok = Parse("state 1 FenceCornerGroggy", c).empty() && c.verb == Verb::State && c.slot == 1 && c.name == "FenceCornerGroggy" && IsVerb("state");
    ok = ok && Parse("state 0 StateFenceCornerGroggy", c).empty() && c.name == "FenceCornerGroggy";  // the game's yaseChgState drops a leading "State"
    ok = ok && Parse("state 8 Idle_2", c).empty() && c.slot == 8 && c.name == "Idle_2";
    Check(ok, "state.parse_good", "slot 1 / the State prefix dropped / slot 8 with _ and a digit");
    bool all_bad = true;
    for (const char* t : {"state", "state 1", "state 9 Idle", "state x Idle", "state 1 Bad-Name", "state 1 Idle now", "state -1 Idle",
                          "state 1 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})
      if (Parse(t, c).empty()) all_bad = false;
    Check(all_bad, "state.parse_bad", "no slot / no name / slot 9 / bad characters / two names / 64+ characters");
    Check(Parse(std::string("state 1 ") + std::string(63, 'A'), c).empty(), "state.parse_63_characters_ok");
  }
  // the registry, written as the game's literal numbers: char+3148 = sm, [sm+40] = desc, [desc] = 4 records, [desc+4] = the array, record i = {ctor, name ptr} at +8i
  auto Reg = []() {
    Fake f = Match();
    const uint32_t ch = f.ChOf(0), sm = ch + 0x8000, desc = ch + 0x9000, recs = ch + 0xA000;
    f.mem[ch + 3148] = sm;
    f.mem[sm + 40] = desc;
    f.mem[desc] = 4;
    f.mem[desc + 4] = recs;
    const char* names[4] = {"Fence", "Idle", "FenceCornerGroggy", "FenceCornerGroggyDamage"};
    for (uint32_t i = 0; i < 4; ++i) {
      f.mem[recs + 8 * i] = 0x82001000u + i;          // a constructor (not the name)
      f.mem[recs + 8 * i + 4] = 0x30000000u + 0x100 * i;  // the name pointer
      for (uint32_t k = 0;; ++k) { f.Put8(0x30000000u + 0x100 * i + k, uint8_t(names[i][k])); if (!names[i][k]) break; }
    }
    f.mem[ch + 28] = ch + 0xC000;  // [[char+28]] = a u16 act (big-endian bytes)
    f.Put8(ch + 0xC000, 0x00); f.Put8(ch + 0xC001, 0x7B);
    return f;
  };
  {
    Fake f = Reg(); State st;
    std::string r = Do(f, st, "state 0 FenceCornerGroggy");
    Check(Starts(r, "ok"), "state.ok_path", r);
    Check(f.state_calls.size() == 1 && f.calls.size() == 1 && f.calls[0].first == int(Fn::QueueState), "state.exactly_one_call", std::to_string(f.state_calls.size()));
    Check(f.state_calls.size() == 1 && f.state_calls[0][0] == f.ChOf(0) + 0x8000 && f.state_calls[0][1] == 0x30000200u && f.state_calls[0][2] == 0,
          "state.call_gets_the_state_machine_and_the_records_own_name_pointer", f.state_calls.empty() ? "no call" : std::to_string(f.state_calls[0][0]) + " " + std::to_string(f.state_calls[0][1]));
    Check(Last(f).find("state 0 FenceCornerGroggy: queued") != std::string::npos && Last(f).find("later tick") != std::string::npos && Last(f).find("record 2") != std::string::npos && Last(f).find("30000200") != std::string::npos &&
              Last(f).find("act was 0x0000007B") != std::string::npos,
          "state.log_has_slot_name_pointer_record_and_act", Last(f));
    Fake g = Reg();
    Check(Starts(Do(g, st, "state 0 StateIdle"), "ok") && g.state_calls.size() == 1 && g.state_calls[0][1] == 0x30000100u, "state.prefix_dropped_finds_the_record", g.state_calls.empty() ? "no call" : "");
    Fake h = Reg();  // only slot 0 has a registry: slot 1 (no state machine) is refused, never given slot 0's
    Check(Starts(Do(h, st, "state 1 Fence"), "refused") && h.calls.empty(), "state.other_slot_is_not_given_slot_0s_registry");
    Fake i0 = Reg();
    // the game's own search never finds record 0 or the last (exclusive bounds) and queues the last for a miss: both are refused here, nothing called
    Check(Starts(Do(i0, st, "state 0 Fence"), "refused") && i0.calls.empty(), "state.record_0_refused");
    Check(Starts(Do(i0, st, "state 0 FenceCornerGroggyDamage"), "refused") && i0.calls.empty(), "state.last_record_refused");
    // already in the state ([[desc+12]+12] == the found index): nothing is queued
    Fake cur = Reg();
    cur.mem[cur.ChOf(0) + 0x9000 + 12] = cur.ChOf(0) + 0xD000;
    cur.mem[cur.ChOf(0) + 0xD000 + 12] = 2;
    std::string rc = Do(cur, st, "state 0 FenceCornerGroggy");
    Check(Starts(rc, "ok") && cur.calls.empty() && Last(cur).find("already in it") != std::string::npos, "state.already_in_it_queues_nothing", rc);
    cur.mem[cur.ChOf(0) + 0xD000 + 12] = 1;
    Check(Starts(Do(cur, st, "state 0 FenceCornerGroggy"), "ok") && cur.state_calls.size() == 1, "state.another_current_state_queues");
  }
  {
    Fake f = Reg(); State st;
    for (const char* t : {"state 0 Nope", "state 0 FenceCorner", "state 0 Fence_", "state 0 fencecornergroggy", "state 0 FenceCornerGroggyDamageX"}) {
      std::string r = Do(f, st, t);
      Check(Starts(r, "refused") && f.calls.empty() && Last(f).find("refused (no such state") != std::string::npos, (std::string("state.unknown_name_refused_no_call ") + t).c_str(), r);
    }
  }
  {
    Fake f = Reg(); State st; f.mem[f.ChOf(0) + 0x8000 + 40] = 0;
    Check(Starts(Do(f, st, "state 0 Idle"), "refused") && f.calls.empty(), "state.no_registry_refused");
    Fake a = Reg(); a.mem[a.ChOf(0) + 3148] = 0;
    Check(Starts(Do(a, st, "state 0 Idle"), "refused") && a.calls.empty(), "state.no_state_machine_refused");
    Fake b = Reg(); b.mem[b.ChOf(0) + 0x9000] = 0;
    Check(Starts(Do(b, st, "state 0 Idle"), "refused") && b.calls.empty(), "state.empty_registry_refused");
    Fake d = Reg(); d.mem[d.ChOf(0) + 0x9000] = 100000;
    Check(Starts(Do(d, st, "state 0 Nope"), "refused") && d.calls.empty(), "state.absurd_count_refused");
    Fake e2 = Reg();
    Check(Starts(Do(e2, st, "state 5 Idle"), "refused") && e2.calls.empty(), "state.empty_slot_refused");
    Check(Starts(Do(e2, st, "state 6 Idle"), "refused") && e2.calls.empty(), "state.referee_refused");
    Fake m = Reg(); m.gate.clock_started = false;
    Check(Starts(Do(m, st, "state 0 Idle"), "busy") && m.calls.empty(), "state.before_the_bell_is_busy");
  }
  {
    Fake f = Reg(); State st; f.dev = false;
    std::string r = Do(f, st, "state 0 FenceCornerGroggy");
    Check(Starts(r, "refused: state is a dev-build verb") && f.calls.empty(), "state.player_build_refused_no_calls", r);
    f.gate.clock_started = false;
    Check(Starts(Do(f, st, "state 0 FenceCornerGroggy"), "refused: state is a dev-build verb"), "state.player_build_refused_before_the_match_gate");
  }
  // ---- place (S110 PLACE: dev only; the game's own yaw / place / floor / warp calls in that order; the place byte put back; the controller angle; the refusals call nothing) ----
  {
    Cmd c;
    bool ok = Parse("place 1 520 525 225", c).empty() && c.verb == Verb::Place && c.slot == 1 && c.x == 520.0f && c.z == 525.0f && c.deg == 225.0f && c.has_facing && IsVerb("place");
    ok = ok && Parse("place 0 -12.5 +3.25", c).empty() && c.x == -12.5f && c.z == 3.25f && !c.has_facing;
    ok = ok && Parse("place 8 2000 -2000 -360", c).empty() && c.deg == -360.0f;
    Check(ok, "place.parse_good", "with and without a facing, signed decimals, the bounds");
    bool all_bad = true;
    for (const char* t : {"place", "place 1", "place 1 5", "place 9 1 1", "place x 1 1", "place 1 a 1", "place 1 1e3 1", "place 1 2000.5 1", "place 1 1 -2001", "place 1 1 1 361", "place 1 1 1 -360.5",
                          "place 1 1 1 1 1", "place 1 - 1", "place 1 . 1", "place 1 0x10 1"})
      if (Parse(t, c).empty()) all_bad = false;
    Check(all_bad, "place.parse_bad", "missing words, slot 9, not decimals, past the typo bounds, five numbers");
  }
  // slot 1 as the wrestler: kind 4 at +24, controller at +3096, skeleton root at +176; a rotation vector at +144..+159 with a sentinel in each word
  auto Pl = []() {
    Fake f = Match();
    const uint32_t ch = f.ChOf(1);
    f.mem[ch + 24] = 4; f.mem[ch + 3096] = ch + 0x8100; f.mem[ch + 176] = ch + 0x8200;
    for (uint32_t i = 0; i < 16; i += 4) f.mem[ch + 144 + i] = 0x11110000u + i;
    f.mem[ch + 148] = 0x3F800000u;  // facing 1.0 rad
    return f;
  };
  {
    Fake f = Pl(); State st; const uint32_t ch = f.ChOf(1), ctrl = ch + 0x8100;
    const std::string r = Do(f, st, "place 1 540 545 225");
    Check(Starts(r, "ok"), "place.ok_path", r);
    Check(f.pcalls.size() == 4 && f.pcalls[0] == "yaw" && f.pcalls[1] == "place" && f.pcalls[2] == "floor" && f.pcalls[3] == "warp", "place.calls_in_the_notes_order_yaw_place_floor_warp",
          std::to_string(f.pcalls.size()));
    Check(f.pargs.size() == 4 && std::fabs(f.pargs[0] - 225.0 * 3.14159265358979323846 / 180.0) < 1e-6, "place.facing_is_degrees_to_radians_in_f1", f.pargs.empty() ? "" : std::to_string(f.pargs[0]));
    Check(f.pargs.size() == 4 && f.pargs[1] == 540.0 && f.pargs[2] == -120.0 && f.pargs[3] == 545.0, "place.warp_gets_x_floor_y_z_in_f1_f2_f3", f.pargs.size() == 4 ? std::to_string(f.pargs[2]) : "");
    Check(f.calls.size() == 4 && f.calls[0].second == ch && f.calls[1].second == ch + 160 && f.calls[2].second == ch && f.calls[3].second == ch, "place.call_arguments_are_the_character_and_char_plus_160");
    Check(f.F32(ch + 160) == 540.0f && f.F32(ch + 168) == 545.0f && f.F32(ch + 164) == -120.0f, "place.position_written");
    Check(f.F32(ch + 0x8200 + 0) == 540.0f && f.F32(ch + 0x8200 + 8) == 545.0f, "place.root_node_x_z_stored_before_the_height_call");
    Check(f.U8(ch + 70) == 7, "place.place_byte_restored_after_the_warp_scene_rule", std::to_string(f.U8(ch + 70)));
    Check(f.mem[ctrl + 48] == f.mem[ch + 160] && f.mem[ctrl + 56] == f.mem[ch + 168], "place.controller_position_mirrored");
    Check(f.mem[ctrl + 32] == f.mem[ch + 148] && f.mem[ctrl + 32] != 0, "place.controller_angle_written");
    Check(f.mem[ch + 208] == f.mem[ch + 144] && f.mem[ch + 212] == f.mem[ch + 148] && f.mem[ch + 216] == f.mem[ch + 152] && f.mem[ch + 220] == f.mem[ch + 156], "place.old_rotation_copied");
    Check(Last(f).find("place 1 540.00 545.00 225.00") != std::string::npos && Last(f).find("read back x 540.000 y -120.000 z 545.000") != std::string::npos && Last(f).find("place 7") != std::string::npos &&
              Last(f).find("facing 3.92699") != std::string::npos, "place.log_has_asked_and_read_back", Last(f));
  }
  {
    Fake f = Pl(); State st; const uint32_t ch = f.ChOf(1);
    const std::string r = Do(f, st, "place 1 -300 10");
    Check(Starts(r, "ok") && f.pcalls.size() == 3 && f.pcalls[0] == "place" && f.mem[ch + 148] == 0x3F800000u && f.U8(ch + 70) == 2 && f.F32(ch + 164) == -605.0f, "place.no_facing_keeps_the_yaw_and_skips_the_call", r);
    Check(f.mem[f.ChOf(1) + 0x8100 + 32] == 0x3F800000u, "place.no_facing_controller_gets_the_kept_yaw");
  }
  {  // each refusal: nothing is called, nothing is stored
    struct Case { const char* name; void (*set)(Fake&); };
    const Case cases[] = {
        {"kind_not_4", [](Fake& f) { f.mem[f.ChOf(1) + 24] = 6; }},
        {"no_controller", [](Fake& f) { f.mem[f.ChOf(1) + 3096] = 0; }},
        {"partner_836_set", [](Fake& f) { f.mem[f.ChOf(1) + 836] = f.ChOf(2); }},
        {"someone_holds_him", [](Fake& f) { f.mem[f.ChOf(2) + 836] = f.ChOf(1); }},
        {"cutscene_actor", [](Fake& f) { f.mem[f.ChOf(1) + 1340] = 3; f.mem[0x836F4B40u + 12] = 1; }},
        {"two_wrestler_special_1920", [](Fake& f) { f.mem[f.ChOf(1) + 1920] = 1; }},
        {"referee", [](Fake& f) { f.mem[f.ChOf(1) + 3720] = 6; }},
    };
    for (const Case& k : cases) {
      Fake f = Pl(); State st; k.set(f);
      const auto before = f.mem;
      const std::string r = Do(f, st, "place 1 540 545 225");
      Check(Starts(r, "refused") && f.calls.empty() && f.pcalls.empty() && f.mem == before && Last(f).find("refused") != std::string::npos, (std::string("place.refused_no_call_no_store ") + k.name).c_str(), r);
    }
    // reviewer (run 28 / 29 logs: slot 0's char+1340 = 0xFFFFFFFF): the game's own test sub_823CB3F0 reads the table only for 0 <= index < 72; an index of -1 or 72+
    // is "no actor". 4u * 0xFFFFFFFF wraps to the word BEFORE the table: a nonzero word there must not refuse the place.
    for (uint32_t idx : {0xFFFFFFFFu, 72u, 0x40000000u}) {
      Fake g = Pl(); State sg;
      g.mem[g.ChOf(1) + 1340] = idx;
      g.mem[0x836F4B40u - 4] = 0xDEAD; g.mem[0x836F4B40u + 4u * 72u] = 0xDEAD; g.mem[0x836F4B40u] = 0xDEAD;
      const std::string rg = Do(g, sg, "place 1 540 545 225");
      Check(Starts(rg, "ok"), (std::string("place.actor_index_out_of_range_is_no_actor ") + std::to_string(idx)).c_str(), rg);
    }
    Fake e2 = Pl(); State st;
    Check(Starts(Do(e2, st, "place 5 1 1"), "refused") && e2.calls.empty(), "place.empty_slot_refused");
    Fake b = Pl(); b.gate.clock_started = false;
    Check(Starts(Do(b, st, "place 1 1 1"), "busy") && b.calls.empty(), "place.before_the_bell_is_busy");
  }
  {
    Fake f = Pl(); State st; f.dev = false;
    const auto before = f.mem;
    std::string r = Do(f, st, "place 1 540 545 225");
    Check(Starts(r, "refused: place is a dev-build verb") && f.calls.empty() && f.mem == before, "place.player_build_refused_no_calls", r);
    f.gate.clock_started = false;
    Check(Starts(Do(f, st, "place 1 540 545 225"), "refused: place is a dev-build verb"), "place.player_build_refused_before_the_match_gate");
  }
  // ---- fire / skip_replay (S110 FIRETABLE review): parse; `fire` arms the seat behind the match gate; `skip_replay` is served after the
  // finish (the post-match replay plays while G+472 != 0, in sequence 9) and is busy outside the in-game sequence ----
  {
    Cmd c;
    bool ok = Parse("fire 0", c).empty() && c.verb == Verb::Fire && c.slot == 0 && IsVerb("fire");
    ok = ok && Parse("fire 8", c).empty() && c.verb == Verb::Fire && c.slot == 8;
    ok = ok && Parse("skip_replay", c).empty() && c.verb == Verb::SkipReplay && IsVerb("skip_replay");
    Check(ok, "fire.parse_good", "fire 0, fire 8, skip_replay");
    bool bad = true;
    for (const char* t : {"fire", "fire 9", "fire x", "fire 0 1", "skip_replay 1", "skip_replay now"})
      bad = bad && !Parse(t, c).empty();
    Check(bad, "fire.parse_bad", "6 bad forms are all errors");
  }
  {
    namespace tf = ::wwe2k14::table_fire;
    tf::Clear();
    Fake f = Match(); State st;
    const std::string r = Do(f, st, "fire 1");
    Check(Starts(r, "ok: fire 1 (armed") && tf::FireState(1) == 0 && tf::FireState(0) == -1, "fire.arms_the_seat", r);
    Check(Last(f).find("director: fire 1: armed") != std::string::npos, "fire.logged", Last(f));
    Fake g = Match();
    Check(Starts(Do(g, st, "fire 6"), "refused") && tf::FireState(6) == -1, "fire.referee_refused");
    Check(Starts(Do(g, st, "fire 5"), "refused") && tf::FireState(5) == -1, "fire.empty_slot_refused");
    Fake b = Match(); b.gate.clock_started = false;
    Check(Starts(Do(b, st, "fire 2"), "busy") && tf::FireState(2) == -1, "fire.before_the_bell_is_busy");
    Fake d = Match(); d.gate.g472 = 2;  // decided: the post-match replay plays now
    const std::string s = Do(d, st, "skip_replay");
    Check(Starts(s, "ok: skip_replay") && tf::g_skip.load(), "skip_replay.served_after_the_finish", s);
    tf::Clear();
    Fake m = Match(); m.gate.seq = 75;
    const std::string u = Do(m, st, "skip_replay");
    Check(Starts(u, "busy: no match is running") && !tf::g_skip.load(), "skip_replay.busy_outside_the_match", u);
    tf::Clear();
  }
  {  // S110 FIRETABLE fix 2: the game's replay lock [[0x8376AAF0]+0x129A8] != 0 (IARP never starts, its skip refuses): skip_replay says so, nothing armed
    namespace tf = ::wwe2k14::table_fire;
    tf::Clear();
    Fake k = Match(); State st;
    k.mem[0x8376AAF0u] = 0x40000000u;
    k.mem[0x40000000u + 0x129A8u] = 1;
    const std::string r = Do(k, st, "skip_replay");
    Check(Starts(r, "refused: the game's replay lock") && !tf::g_skip.load(), "skip_replay.refused_while_the_game_locks_replays", r);
    k.mem[0x40000000u + 0x129A8u] = 0;
    Check(Starts(Do(k, st, "skip_replay"), "ok: skip_replay") && tf::g_skip.load(), "skip_replay.served_when_the_lock_is_clear");
    tf::Clear();
  }
  // ---- finisher (S110 FINPICK): parse; the wish reaches the glue with the slot's character; the refusals and the bell call nothing ----
  {
    Cmd c;
    bool ok = Parse("finisher 0 hold", c).empty() && c.verb == Verb::Finisher && c.slot == 0 && c.value == 1 && IsVerb("finisher");
    ok = ok && Parse("finisher 8 nohold", c).empty() && c.verb == Verb::Finisher && c.slot == 8 && c.value == 2;
    ok = ok && Parse("finisher 1 any", c).empty() && c.verb == Verb::Finisher && c.value == 0;
    Check(ok, "finisher.parse_good", "hold 1, nohold 2, any 0");
    bool bad = true;
    for (const char* t : {"finisher", "finisher 0", "finisher 9 hold", "finisher x hold", "finisher 0 Hold", "finisher 0 17", "finisher 0 hold extra"})
      bad = bad && !Parse(t, c).empty();
    Check(bad, "finisher.parse_bad", "7 bad forms are all errors");
  }
  {
    Fake f = Match(); State st;
    const std::string r = Do(f, st, "finisher 1 nohold");
    Check(Starts(r, "ok: finisher 1 nohold (fake fit)") && f.wishes.size() == 1 && f.wishes[0].slot == 1 && f.wishes[0].ch == f.ChOf(1) && f.wishes[0].wish == 2,
          "finisher.wish_reaches_the_glue_with_the_character", r);
    Check(Last(f).find("director: finisher 1 nohold (fake fit; char ") != std::string::npos, "finisher.logged", Last(f));
    Check(Starts(Do(f, st, "finisher 2 hold"), "ok") && f.wishes.size() == 2 && f.wishes[1].wish == 1, "finisher.hold_is_1");
    Check(Starts(Do(f, st, "finisher 2 any"), "ok") && f.wishes.size() == 3 && f.wishes[2].wish == 0, "finisher.any_reaches_the_glue");
    Fake g = Match();
    Check(Starts(Do(g, st, "finisher 6 hold"), "refused") && g.wishes.empty(), "finisher.referee_refused");
    Check(Starts(Do(g, st, "finisher 5 hold"), "refused") && g.wishes.empty() && Last(g).find("refused") != std::string::npos, "finisher.empty_slot_refused");
    Fake b = Match(); b.gate.clock_started = false;
    Check(Starts(Do(b, st, "finisher 1 hold"), "busy") && b.wishes.empty(), "finisher.before_the_bell_is_busy");
  }
  // ---- omg (S110 OMGVERB): parse; the wish reaches the glue with the slot's character; the refusals and the bell call nothing ----
  {
    Cmd c;
    bool ok = Parse("omg 0 ring", c).empty() && c.verb == Verb::Omg && c.slot == 0 && c.on && IsVerb("omg");
    ok = ok && Parse("omg 8 off", c).empty() && c.verb == Verb::Omg && c.slot == 8 && !c.on;
    Check(ok, "omg.parse_good", "ring on, off");
    bool bad = true;
    for (const char* t : {"omg", "omg 0", "omg 9 ring", "omg x ring", "omg 0 Ring", "omg 0 on", "omg 0 ring extra"})
      bad = bad && !Parse(t, c).empty();
    Check(bad, "omg.parse_bad", "7 bad forms are all errors");
  }
  {
    Fake f = Match(); State st;
    const std::string r = Do(f, st, "omg 1 ring");
    Check(Starts(r, "ok: omg 1 ring (fake gates)") && f.omgs.size() == 1 && f.omgs[0].slot == 1 && f.omgs[0].ch == f.ChOf(1) && f.omgs[0].on,
          "omg.wish_reaches_the_glue_with_the_character", r);
    Check(Last(f).find("director: omg 1 ring (fake gates; char ") != std::string::npos, "omg.logged", Last(f));
    Check(Starts(Do(f, st, "omg 1 off"), "ok: omg 1 off") && f.omgs.size() == 2 && !f.omgs[1].on, "omg.off_reaches_the_glue");
    Fake g = Match();
    Check(Starts(Do(g, st, "omg 6 ring"), "refused") && g.omgs.empty(), "omg.referee_refused");
    Check(Starts(Do(g, st, "omg 5 ring"), "refused") && g.omgs.empty(), "omg.empty_slot_refused");
    Fake b = Match(); b.gate.clock_started = false;
    Check(Starts(Do(b, st, "omg 1 ring"), "busy") && b.omgs.empty(), "omg.before_the_bell_is_busy");
  }
  std::printf("SUMMARY %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
