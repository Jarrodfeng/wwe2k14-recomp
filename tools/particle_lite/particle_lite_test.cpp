// S102 lane T18 offline proof (0 game runs): the SHIPPED src\renderer_adapter\particle_lite.h gate driven over the op sequence of a real capture (tools\particle_lite\lite_trace.py).
//
//   particle_lite_test TRACE [--plant N] [--mutate tex|shader|gap|tex_unmarked|header|z] [--bundle default] [--marks live] [--bench] [--burst A:B] [--census]
//
// The model: a transcription of what DrawCommon does with the gate (reader.h, lines marked "S102 T18"; tools\particle_lite\lite_check.py --part wiring pins the text it is
// transcribed from). For every draw of the trace the FULL path's record is the trace's own record (the capture is what the full path recorded). The LITE path's record takes the fields
// DrawCommon copies (fixed, dynamic, texture slots, layout, the origin) from the run's previous draw's record, and its own bank delta, bool / loop, geometry (what stays per draw).
// Requirement: the lite stream equals the full stream, draw by draw, on every field. `unmarked` is not in a capture: it is modelled as "no state, shader or layout record changed since the
// previous draw". The texture slots are NOT in it (review F1): the marks (NoMarks) read only the high word of dev+24 (register block 0x2280), the texture slot dirty bits are in the LOW word,
// so a slot change leaves a draw "unmarked"; the gate's slot snapshot (words + object, header bytes, flags, epoch) must catch it. The model's snapshot = the draw's slot digest (sw), a
// header part (sh, 0 in a capture) -- a change in either breaks the run. Draw ENTRIES (a draw that enters a hook and records nothing: a "Z" trace line) are counted like the reader's.
//
// Planted twins (each must fail; --mutate injects the situation the plant mishandles, and a clean gate must pass the same mutated trace):
//   plant 1 + mutate tex     the run is not broken by a texture change (marks ignored)
//   plant 2 + mutate shader  a different shader is merged into the run (the key does not compare the shaders; the device's marks missed the bind)
//   plant 3 + mutate gap     another op between two draws does not end the run
//   plant 4                  a bank delta is dropped (the lite record takes the previous draw's bank)
//   plant 5 + mutate tex     the texture slot records are not copied (the lite record keeps its own empty ones)
//   plant 6 + mutate tex_unmarked   the slot snapshot is ignored (a slot change with the marks clear does not break the run)
//   plant 7 + --bundle default      the thin query is skipped after a draw that DID query (the Query count differs from the full path's)
//   plant 8 + mutate z       a draw entry with no record between two candidates does not end the run
//   plant 9 + mutate header  the snapshot ignores the header part (a texture header rewrite does not break the run)
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define T18_PLANTS 1
#include "../../src/renderer_adapter/particle_lite.h"

namespace pl = rdr_adapter::particle_lite;

struct Rec {
  bool cand = false;
  uint64_t vs = 0, ps = 0;
  uint32_t prim = 0, flags = 0, fixed = 0, dynamic = 0, layout = 0, bool_loop = 0;
  uint64_t slots = 0;
  uint32_t site = 0, pass = 0, object = 0, part = 0, widget = 0;
  uint32_t klass = 0, op = 0, tech = 0;  // S102 SW2 review F4: the rest of DrawOrigin (klass 4 = Text: its TakeDrawOrigin pushes a TextRun)
  uint32_t truns = 0;                      // 1 when the full path pushed a TextRun for this draw (a glyph draw: TakeDrawOrigin)
  bool cdest = false;                      // S102 SW2 review F2: a slot image is a copy destination (EnsureContent runs for it every draw)
  uint64_t vsb = 0, psb = 0, geo = 0;
  uint64_t sw = 0, sh = 0;      // the slot snapshot's two parts (sw = words + object + flags: the slot digest; sh = header bytes + epoch: 0 in a capture)
  uint32_t shobj = 0;           // the shader object (not in a capture: 0; the shader mutation sets it)
  bool force_unmarked = false;  // the mutation says the device's marks missed it (the key must catch it)
  bool force_marked = false;    // the mutation says a setter ran (the marks must catch it)
};
static bool SameState(const Rec& a, const Rec& b) {
  return a.vs == b.vs && a.ps == b.ps && a.prim == b.prim && a.flags == b.flags && a.fixed == b.fixed && a.dynamic == b.dynamic && a.layout == b.layout && a.bool_loop == b.bool_loop && a.shobj == b.shobj;
}
static bool SameRec(const Rec& a, const Rec& b, const char** what) {
#define F(x)                \
  if (a.x != b.x) {         \
    *what = #x;             \
    return false;           \
  }
  F(klass) F(op) F(tech) F(truns) F(vs) F(ps) F(prim) F(flags) F(fixed) F(dynamic) F(layout) F(bool_loop) F(slots) F(site) F(pass) F(object) F(part) F(widget) F(vsb) F(psb) F(geo)
#undef F
  return true;
}

struct Op {
  bool is_z = false;  // a draw entry that recorded nothing (an early return after ReadMarks)
  bool is_draw = false;
  Rec r;
};
struct Frame {
  long capture = 0;
  std::vector<Op> ops;
};

static bool g_all_cands = false;  // S102 SW2 (--general): every draw is a candidate
static bool Load(const char* path, std::vector<Frame>& out) {
  FILE* f = std::fopen(path, "r");
  if (!f) return false;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == 'F') {
      Frame fr;
      long g = 0;
      std::sscanf(line, "F %ld %ld", &fr.capture, &g);
      out.push_back(fr);
    } else if (line[0] == 'Z') {
      if (out.empty()) return false;
      Op z;
      z.is_z = true;
      out.back().ops.push_back(z);
    } else if (line[0] == 'X') {
      if (out.empty()) return false;
      out.back().ops.push_back(Op{});
    } else if (line[0] == 'D') {
      if (out.empty()) return false;
      Op o;
      o.is_draw = true;
      Rec& r = o.r;
      int cand = 0;
      unsigned long long vs, ps, slots, vsb, psb, geo;
      unsigned site, pass, object, tech = 0;
      int n = std::sscanf(line, "D %d %llx %llx %u %u %u %u %u %u %llx %x %x %x %u %u %llx %llx %llx %u %u %x", &cand, &vs, &ps, &r.prim, &r.flags, &r.fixed, &r.dynamic, &r.layout, &r.bool_loop,
                          &slots, &site, &pass, &object, &r.part, &r.widget, &vsb, &psb, &geo, &r.klass, &r.op, &tech);
      if (n != 21) return false;  // (a trace made before SW2 has 18 fields: made again by lite_trace.py)
      r.tech = tech;
      r.truns = r.klass == 4 ? 1 : 0;
      r.cand = cand != 0 || g_all_cands, r.vs = vs, r.ps = ps, r.slots = slots, r.sw = slots, r.site = site, r.pass = pass, r.object = object, r.vsb = vsb, r.psb = psb, r.geo = geo;
      out.back().ops.push_back(o);
    }
  }
  std::fclose(f);
  return !out.empty();
}

struct Args {
  const char* trace = nullptr;
  int plant = 0;
  std::string mutate;
  long burst_a = -1, burst_b = -1;
  bool census = false, bundle = false, live_marks = false, bench = false;
  bool general = false;  // S102 SW2: every draw of the trace is a candidate of the general gate (origin not in the key)
};

// Mutations: applied to the truth (the "full" record) before either path sees it. Every kth candidate.
static void Mutate(std::vector<Frame>& frames, const std::string& m) {
  if (m.empty()) return;
  uint64_t n = 0;
  for (Frame& fr : frames) {
    for (size_t i = 0; i < fr.ops.size(); ++i) {
      Op& o = fr.ops[i];
      if (!o.is_draw || !o.r.cand) continue;
      if (++n % 7) continue;
      if (m == "tex") {  // a different texture for this draw: a setter ran (marked), and the recorded slots differ
        o.r.slots ^= 0x5A5A5A5Aull;
        o.r.sw = o.r.slots;
        o.r.force_marked = true;
      } else if (m == "cpu_write") {  // S102 SW2 F2: a copy-destination image rewritten between two draws: the full path's EnsureContent runs for it on every draw
        o.r.cdest = true;
      } else if (m == "state") {  // a setter changed the render state (marked): the recorded fixed state differs
        o.r.fixed += 1000;
        o.r.force_marked = true;
      } else if (m == "tex_unmarked") {  // a texture changed and NO mark says so (the slot dirty bits are not in what NoMarks reads): only the snapshot can catch it
        o.r.slots ^= 0x5A5A5A5Aull;
        o.r.sw = o.r.slots;
        o.r.force_unmarked = true;
      } else if (m == "header") {  // a texture header rewritten in place (words and object unchanged), marks clear: the record's slot data moved, the snapshot's header part says so
        o.r.slots ^= 0x3C3C3C3Cull;
        o.r.sh = 0x3C3C3C3Cull;
        o.r.force_unmarked = true;
      } else if (m == "z" && i > 0 && fr.ops[i - 1].is_draw) {  // a draw entered a hook and recorded nothing (its setters ran, its marks were cleared): the next draw's state moved, its marks read clear
        Op z;
        z.is_z = true;
        fr.ops.insert(fr.ops.begin() + long(i), z);
        fr.ops[i + 1].r.fixed += 1000;
        fr.ops[i + 1].r.force_unmarked = true;
        ++i;
      } else if (m == "shader") {  // another shader object bound: its state records differ; the marks missed it (the key must break the run)
        o.r.shobj = 0x77;
        o.r.fixed += 1000;
        o.r.dynamic += 1000;
        o.r.force_unmarked = true;
      } else if (m == "gap" && i > 0 && fr.ops[i - 1].is_draw) {  // an op between two draws (a pass change): the next draw's records moved; the marks missed it
        Op x;
        fr.ops.insert(fr.ops.begin() + long(i), x);
        fr.ops[i + 1].r.fixed += 1000;
        fr.ops[i + 1].r.force_unmarked = true;
        ++i;
      }
    }
  }
}

struct Stats {
  uint64_t draws = 0, cands = 0, taken = 0, runs = 0, mismatches = 0, q_full = 0, q_lite = 0, ens_full = 0, ens_lite = 0;
  uint64_t by[uint8_t(pl::Reason::Count)] = {};
};

static void RunModel(const std::vector<Frame>& frames, const Args& A, Stats& all, Stats& burst, const char** first_mismatch, long* first_frame) {
  pl::Gate gate;
  gate.plant = A.plant;
  gate.general = A.general;  // S102 SW2
  gate.census = A.general ? &pl::g_census_gen : &pl::g_census;
  uint64_t generation = 0;
  uint64_t entries = 0;  // the reader's draw-entry counter (ReadMarks calls)
  for (const Frame& fr : frames) {
    ++generation;
    const bool in_burst = A.burst_a >= 0 && fr.capture >= A.burst_a && fr.capture <= A.burst_b;
    std::vector<Rec> out;  // the lite path's recorded draws (the model of t.draws)
    uint64_t ops = 0;
    const Rec* prev_truth = nullptr;  // the previous op's truth, when that op was a draw
    bool last_in_run = false;
    uint64_t runs_here = 0;
    for (const Op& o : fr.ops) {
      if (o.is_z) {
        ++entries;
        prev_truth = nullptr;
        last_in_run = false;
        continue;
      }
      if (!o.is_draw) {
        ++ops;
        prev_truth = nullptr;
        last_in_run = false;
        continue;
      }
      const Rec& t = o.r;
      Rec rec = t;
      ++entries;
      ++all.draws, burst.draws += in_burst;
      if (t.cand) {
        ++all.cands, burst.cands += in_burst;
        bool unmarked = prev_truth && SameState(*prev_truth, t);
        if (t.force_unmarked) unmarked = true;
        if (t.force_marked) unmarked = false;
        if (A.live_marks) unmarked = false;  // PC run 7245: the recorder raises a mark on nearly every particle draw (29,174 of 29,704): every candidate carries the marks
        pl::Key k;
        k.site = t.site, k.pass_site = t.pass, k.object = t.object, k.part = t.part, k.widget = t.widget;
        k.origin_ok = t.site != 0 || t.object != 0 || t.pass != 0;  // (a draw whose origin did not resolve has no site: the model's stand-in)
        k.op = t.op;
        k.run = t.klass == 4 ? 1 : 0;  // stand-in: a Text-class draw is a glyph draw (a TextRun run key)
        k.vs_object = t.shobj, k.ps_object = t.shobj, k.decl = t.layout, k.prim = t.prim;
        k.vs = reinterpret_cast<const void*>(uintptr_t(t.vs)), k.ps = reinterpret_cast<const void*>(uintptr_t(t.ps));
        const bool noquery = ((t.site ^ t.pass ^ t.object ^ t.part ^ t.widget) % 3) == 0;  // the bundle arm's thin model: the answer is a function of the origin key (an object class that returns before Query)
        all.q_full += noquery ? 0 : 1;
        auto slots_same = [&] {
          const Rec& p = out[gate.draw_index];  // the previous record's snapshot against the live one (this draw's truth)
          return p.sw == t.sw && (A.plant == 9 || p.sh == t.sh);
        };
        auto state_same = [&] { return SameState(out[gate.draw_index], t); };  // the state words gathered now against the state cache's (= the previous draw's records)
        all.ens_full += t.cdest;
        const pl::Reason r = gate.Decide(k, unmarked, generation, out.size(), ops, false, entries, state_same, slots_same);
        all.q_lite += (r == pl::Reason::Taken && (!A.general || gate.origin_same) && gate.SkipThin()) || noquery ? 0 : 1;
        all.ens_lite += (r == pl::Reason::Taken && A.plant == 13) ? 0 : t.cdest;  // the reader calls EnsureContent for a taken draw's copied slots (plant 13: it does not)
        ++all.by[uint8_t(r)], burst.by[uint8_t(r)] += in_burst;
        if (r == pl::Reason::Taken) {
          const Rec& p = out[gate.draw_index];
          rec.fixed = p.fixed, rec.dynamic = p.dynamic, rec.layout = p.layout;  // copied from the run's previous record
          rec.slots = A.plant == 5 ? 0 : p.slots;
          const bool origin_ok = !A.general || (gate.origin_same && k.run == 0 && k.widget == 0 && k.text == 0) || A.plant == 11 || (A.plant == 12 && gate.origin_same);  // review F1 guard; plant 12 drops it
          if (origin_ok) rec.site = p.site, rec.pass = p.pass, rec.object = p.object, rec.part = p.part, rec.widget = p.widget, rec.klass = p.klass, rec.op = p.op, rec.tech = p.tech, rec.truns = 0;  // a copied origin pushes no TextRun (TakeDrawOrigin is skipped)  // the origin record (general gate: only when the origin key is equal; plant 11 copies it always)
          if (A.general && origin_ok) ++pl::g_census_gen.origin_copied;
          if (A.plant == 4) rec.vsb = p.vsb;                                                                     // the bank delta dropped
          ++all.taken, burst.taken += in_burst;
          if (!last_in_run) ++all.runs, runs_here += in_burst, burst.runs += in_burst;
          last_in_run = true;
        } else {
          last_in_run = false;
        }
        out.push_back(rec);
        ++ops;
        pl::SlotsSnap snap;
        snap.epoch = 0, snap.n = 0;
        if (r == pl::Reason::Taken) gate.RememberTaken(k, out.size() - 1, ops, noquery, entries);  // S102 SW2: the shipped call for a taken draw
        else gate.Remember(k, generation, out.size() - 1, ops, noquery, entries, snap);
      } else {
        out.push_back(rec);
        ++ops;
        last_in_run = false;
      }
      const char* what = "";
      if (!SameRec(rec, t, &what)) {
        ++all.mismatches;
        if (!*first_mismatch) *first_mismatch = what, *first_frame = fr.capture;
      }
      prev_truth = &t;
    }
  }
}

static int fails = 0;
static void Check(bool ok, const char* name, const char* detail = "") {
  std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, *detail ? ": " : "", detail);
  if (!ok) ++fails;
}

// Synthetic units on the gate's own rules (no capture needed).
static pl::Reason Dec(pl::Gate& g, const pl::Key& k, bool unmarked, uint64_t gen, uint64_t draws, uint64_t ops, bool excl, uint64_t entries, bool slots_same = true, bool state_same = true) {
  return g.Decide(k, unmarked, gen, draws, ops, excl, entries, [&] { return state_same; }, [&] { return slots_same; });
}
static void Units() {
  pl::Gate g;
  pl::Key k;
  pl::SlotsSnap snap;
  snap.epoch = 0, snap.n = 0;
  k.site = pl::kSpriteSite, k.vs = reinterpret_cast<const void*>(1), k.ps = reinterpret_cast<const void*>(2), k.vs_object = 5, k.ps_object = 6, k.prim = 6;
  Check(Dec(g, k, true, 1, 0, 0, false, 1) == pl::Reason::FirstOfRun, "unit: the first candidate of a frame part is the full path");
  g.Remember(k, 1, 0, 1, false, 1, snap);
  Check(Dec(g, k, true, 1, 1, 1, false, 2) == pl::Reason::Taken, "unit: the next adjacent equal draw is taken");
  Check(Dec(g, k, false, 1, 1, 1, false, 2, true, false) == pl::Reason::Marked, "unit: a marked draw whose state words moved is not taken");
  {
    const uint64_t before = pl::g_census.marked_equal;
    Check(Dec(g, k, false, 1, 1, 1, false, 2, true, true) == pl::Reason::Taken && pl::g_census.marked_equal == before + 1, "unit: a marked draw whose state words are equal IS taken, and counted as marked but equal");
  }
  Check(Dec(g, k, true, 1, 1, 2, false, 2) == pl::Reason::Gap, "unit: an op in between is a gap");
  Check(Dec(g, k, true, 1, 2, 1, false, 2) == pl::Reason::Gap, "unit: a draw in between is a gap");
  Check(Dec(g, k, true, 1, 1, 1, false, 3) == pl::Reason::Gap, "unit (F2): a draw entry that recorded nothing in between is a gap, the tables saying nothing moved");
  Check(Dec(g, k, true, 1, 1, 1, false, 1) == pl::Reason::Gap, "unit (F2): an entry count that did not advance is not the next draw either");
  Check(Dec(g, k, true, 1, 1, 1, false, 2, false) == pl::Reason::SlotsMoved, "unit (F1): a moved slot snapshot with the marks clear is declined");
  Check(Dec(g, k, true, 2, 1, 1, false, 2) == pl::Reason::FirstOfRun, "unit: a new frame part starts again");
  pl::Key k2 = k;
  k2.site = pl::kQuadSite;
  Check(Dec(g, k2, true, 1, 1, 1, false, 2) == pl::Reason::KeyMoved, "unit: another call site is not the run");
  k2 = k, k2.prim = 4;
  Check(Dec(g, k2, true, 1, 1, 1, false, 2) == pl::Reason::KeyMoved, "unit: another primitive is not the run");
  k2 = k, k2.strides[3] = 9;
  Check(Dec(g, k2, true, 1, 1, 1, false, 2) == pl::Reason::KeyMoved, "unit: other strides are not the run");
  k2 = k, k2.object = 77;
  Check(Dec(g, k2, true, 1, 1, 1, false, 2) == pl::Reason::KeyMoved, "unit: another origin object is not the run");
  {  // F4: an excluded draw ends the run
    pl::Gate e;
    Dec(e, k, true, 1, 0, 0, false, 1);
    e.Remember(k, 1, 0, 1, false, 1, snap);
    Check(Dec(e, k, true, 1, 1, 1, true, 2) == pl::Reason::Excluded, "unit (F4): an excluded mode is never taken");
    e.Remember(k, 1, 1, 2, false, 2, snap);
    Check(Dec(e, k, true, 1, 2, 2, false, 3) == pl::Reason::FirstOfRun, "unit (F4): the draw after an excluded one starts a run (its record was not made by the full path under the same modes)");
  }
  {  // F1: the snapshot itself
    pl::SlotsSnap a, b;
    std::memset(&a, 0, sizeof(a));
    a.epoch = 4, a.n = 1, a.s[0].slot = 3, a.s[0].object = 0x1000, a.s[0].w[2] = 7;
    b = a;
    Check(a.Equal(b), "unit (F1): equal snapshots are equal");
    b = a, b.s[0].w[2] = 8;
    Check(!a.Equal(b), "unit (F1): a changed fetch word (the dirty-bit case) differs");
    b = a, b.s[0].header[5] = 1;
    Check(!a.Equal(b), "unit (F1): a rewritten texture header differs");
    b = a, b.epoch = 5;
    Check(!a.Equal(b), "unit (F1): a registry epoch move differs");
    b = a, b.s[0].flags = 2;
    Check(!a.Equal(b), "unit (F1): an image flag flip (copy_dest) differs");
    b = a, b.s[0].object = 0, a.s[0].object = 0;
    a.s[0].header[5] = 9;
    Check(a.Equal(b), "unit (F1): header bytes are not compared when there is no object");
  }
  {  // F6: the thin skip follows the previous draw's answer
    pl::Gate th;
    th.Remember(k, 1, 0, 1, false, 1, snap);
    Check(!th.SkipThin(), "unit (F6): the thin query is not skipped after a draw that queried");
    th.Remember(k, 1, 1, 2, true, 2, snap);
    Check(th.SkipThin(), "unit (F6): the thin query is skipped after a draw that returned before Query");
  }
  {  // S102 SW2: the general gate
    pl::Gate g;
    g.general = true, g.census = &pl::g_census_gen;
    pl::Key a = k;
    a.site = 0x1111, a.object = 7, a.part = 1;
    pl::SlotsSnap sn;
    sn.epoch = 0, sn.n = 0;
    g.Remember(a, 1, 0, 1, false, 1, sn);
    pl::Key b = a;
    b.object = 8;  // another object, same shader pair and state
    Check(Dec(g, b, true, 1, 1, 1, false, 2) == pl::Reason::Taken && !g.origin_same, "unit (general): another origin with the same shader pair and state is taken, origin_same false");
    g.RememberTaken(b, 1, 2, false, 2);
    Check(Dec(g, b, true, 1, 2, 2, false, 3) == pl::Reason::Taken && g.origin_same, "unit (general): the next draw of that origin compares against the NEW origin (RememberTaken kept the moved key)");
    pl::Key c = b;
    c.ps_object = 99;
    Check(Dec(g, c, true, 1, 2, 2, false, 3) == pl::Reason::KeyMoved, "unit (general): another shader object is not the run");
    c = b, c.op = 19;
    Check(Dec(g, c, true, 1, 2, 2, false, 3) == pl::Reason::Taken && !g.origin_same, "unit (general, F3): another origin op is not the same origin");
    c = b, c.origin_ok = false;
    Check(Dec(g, c, true, 1, 2, 2, false, 3) == pl::Reason::Taken && !g.origin_same, "unit (general): a draw whose origin did not resolve never shares an origin record");
    pl::Key d2 = b;
    d2.strides[1] = 4;
    Check(Dec(g, d2, true, 1, 2, 2, false, 3) == pl::Reason::KeyMoved, "unit (general): other strides are not the run");
    Check(Dec(g, b, true, 1, 2, 3, false, 3) == pl::Reason::Gap, "unit (general): an op in between is a gap");
    Check(Dec(g, b, true, 1, 2, 2, false, 3, false) == pl::Reason::SlotsMoved, "unit (general): moved slots decline");
  }
  Check(pl::g_census.runs >= 1 && pl::g_census.taken >= 1, "unit: the census counts a taken draw and its run");
  static char line[500];
  pl::g_on.store(true);
  pl::g_census = pl::Census{};
  pl::g_census.window_start = 100;
  pl::g_census.frames = 4, pl::g_census.taken = 7, pl::g_census.runs = 2, pl::g_census.candidates = 20, pl::g_census.by[1] = 3, pl::g_census.by[2] = 4, pl::g_census.misses = 1, pl::g_census.marked_equal = 2;  // (the call below counts the fifth frame)
  struct L {
    static void Log(const char* s) { std::snprintf(line, sizeof(line), "%s", s); }
  };
  pl::LogCensus(700, &L::Log);
  Check(std::strstr(line, "[rdr][particle_lite] frames 5, taken 7 draws, runs 2, declined 13 (first of a run 3, marked 4, key moved 0, gap 0, excluded 0, slots moved 0), marked but equal 2, origin copied 0, particle_lite_miss 1") != nullptr, "unit: the census line", line);
  pl::g_on.store(false);
}

// Cost of the new compare per candidate (the gate's own Decide with lambdas of the reader's real size): the state words gather (StateWords: 260 bytes of memcpy from the device image, 8 big-endian loads)
// and compare, and the slot snapshot of 2 slots (GPU copy words, header bytes, flags) built and compared. The work the lite path saves is the full path's slot walk, layout lookup, origin record and
// thin query: not measurable offline (the reader needs the game); the R13 row gives 6-7 us for a whole recorded draw.
static void Bench() {
  static uint8_t dev[16384], cache[512], now[512];
  for (size_t i = 0; i < sizeof(dev); ++i) dev[i] = uint8_t(i * 31);
  /* cache is filled after the lambda below */
  volatile uint32_t sink = 0;
  auto gather_compare = [&] {
    std::memcpy(now, dev + 10424, 20), std::memcpy(now + 20, dev + 10460, 140), std::memcpy(now + 160, dev + 10688, 4), std::memcpy(now + 164, dev + 10832, 16), std::memcpy(now + 180, dev + 12288, 4);
    std::memcpy(now + 184, dev + 12816, 20), std::memcpy(now + 204, dev + 13024, 16), std::memcpy(now + 220, dev + 13052, 16);
    uint32_t acc = 0;
    for (int i = 0; i < 8; ++i) {
      uint32_t v;
      std::memcpy(&v, dev + 12816 + 4 * (i % 5), 4);
      acc += __builtin_bswap32(v);
    }
    sink = sink + acc;
    return std::memcmp(now, cache, 236) == 0 || (sink & 1) == 2;
  };
  gather_compare();
  std::memcpy(cache, now, 236);
  pl::SlotsSnap prev, cur;
  std::memset(&prev, 0, sizeof(prev));
  prev.n = 2, prev.s[0].slot = 0, prev.s[1].slot = 4, prev.s[0].object = prev.s[1].object = 0x1000;
  auto snap_compare = [&] {
    cur.epoch = prev.epoch, cur.n = prev.n;
    for (uint32_t i = 0; i < cur.n; ++i) {
      std::memcpy(cur.s[i].w, dev + 100 + 24 * i, 24);
      std::memcpy(cur.s[i].header, dev + 300 + 24 * i, 24);
      cur.s[i].slot = prev.s[i].slot, cur.s[i].object = prev.s[i].object, cur.s[i].image = 0, cur.s[i].flags = 0, std::memset(cur.s[i].pack_host, 0, 4);
    }
    std::memcpy(prev.s[0].w, cur.s[0].w, 24), std::memcpy(prev.s[1].w, cur.s[1].w, 24), std::memcpy(prev.s[0].header, cur.s[0].header, 24), std::memcpy(prev.s[1].header, cur.s[1].header, 24);
    return cur.Equal(prev);
  };
  pl::Key k;
  k.site = pl::kSpriteSite;
  const int N = 2000000;
  struct T {
    const char* name;
    bool unmarked;
  } arms[] = {{"decide, marks clear (slots snapshot only)", true}, {"decide, marks set (state gather + compare, slots snapshot)", false}};
  for (const T& arm : arms) {
    pl::Gate g;
    pl::SlotsSnap empty;
    empty.epoch = 0, empty.n = 0;
    g.Remember(k, 1, 0, 1, false, 1, empty);
    uint64_t taken = 0, entries = 1;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
      ++entries;
      const bool r = g.Decide(k, arm.unmarked, 1, g.draw_index + 1, g.ops_after, false, entries, gather_compare, snap_compare) == pl::Reason::Taken;
      taken += r;
      g.Remember(k, 1, g.draw_index + 1, g.ops_after, false, entries, empty);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
    std::printf("bench: %s: %.1f ns per candidate (taken %" PRIu64 " of %d)\n", arm.name, ns, taken, N);
  }
}

int main(int argc, char** argv) {
  Args A;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s == "--plant" && i + 1 < argc) A.plant = std::atoi(argv[++i]);
    else if (s == "--mutate" && i + 1 < argc) A.mutate = argv[++i];
    else if (s == "--burst" && i + 1 < argc) std::sscanf(argv[++i], "%ld:%ld", &A.burst_a, &A.burst_b);
    else if (s == "--census") A.census = true;
    else if (s == "--general") A.general = true, g_all_cands = true;
    else if (s == "--bench") A.bench = true;
    else if (s == "--marks" && i + 1 < argc) A.live_marks = std::string(argv[++i]) == "live";
    else if (s == "--bundle" && i + 1 < argc) A.bundle = std::string(argv[++i]) == "default";
    else A.trace = argv[i];
  }
  if (!A.trace) {
    std::puts("usage: particle_lite_test TRACE [--plant N] [--mutate tex|shader|gap|tex_unmarked|header|z] [--bundle default] [--burst A:B]");
    return 2;
  }
  std::vector<Frame> frames;
  if (!Load(A.trace, frames)) {
    std::printf("FAIL: cannot read the trace %s\n", A.trace);
    return 1;
  }
  pl::g_plant_c7_no_entry = A.plant == 10;
  Mutate(frames, A.mutate);
  Stats all, burst;
  const char* what = nullptr;
  long wf = 0;
  RunModel(frames, A, all, burst, &what, &wf);
  std::printf("trace %s: %zu frames, %" PRIu64 " draws, %" PRIu64 " candidates, taken %" PRIu64 " in %" PRIu64 " runs (first of a run %" PRIu64 ", marked %" PRIu64 ", key moved %" PRIu64 ", gap %" PRIu64 ", slots moved %" PRIu64 ")\n",
              A.trace, frames.size(), all.draws, all.cands, all.taken, all.runs, all.by[1], all.by[2], all.by[3], all.by[4], all.by[6]);
  if (A.burst_a >= 0)
    std::printf("burst frames %ld-%ld: %" PRIu64 " draws, %" PRIu64 " candidates, taken %" PRIu64 " in %" PRIu64 " runs (first of a run %" PRIu64 ", marked %" PRIu64 ", key moved %" PRIu64 ", gap %" PRIu64 ", slots moved %" PRIu64 ")\n",
                A.burst_a, A.burst_b, burst.draws, burst.cands, burst.taken, burst.runs, burst.by[1], burst.by[2], burst.by[3], burst.by[4], burst.by[6]);
  if (A.live_marks) std::printf("live-mark arm: every candidate carries the marks; taken %" PRIu64 " of %" PRIu64 ", marked but equal %" PRIu64 "\n", all.taken, all.cands, (A.general ? pl::g_census_gen : pl::g_census).marked_equal);
  if (A.bench) Bench();
  if (A.plant == 0 && A.mutate.empty()) Units();
  char d[160];
  std::snprintf(d, sizeof(d), "%" PRIu64 " mismatching draws%s%s", all.mismatches, what ? ", first field " : "", what ? what : "");
  Check(all.mismatches == 0, "lite stream == full stream, every field of every draw", d);
  {  // C7 hole (a shipped path): the state cache is filled by draw A (entry 1); draw X enters a hook, its marks are cleared by the null draw and it returns early (entry 2, nothing recorded);
     // draw B (entry 3) reads no marks. B must not take by_marks. The adjacent draw (entry 2) still may.
    const uint64_t cache_entry = 1;
    Check(!pl::StateEntryOk(cache_entry, 3) && pl::StateEntryOk(cache_entry, 2), "C7 hole: a draw dropped between two recorded ones stops the by_marks state path; adjacent draws still take it");
  }
  if (A.bundle) {  // the default bundle's thin-lite facts table: the query count of the lite path equals the full path's (a skipped query was one the full path did not make)
    char q[120];
    std::snprintf(q, sizeof(q), "full %" PRIu64 " thin queries, lite %" PRIu64, all.q_full, all.q_lite);
    Check(all.q_full == all.q_lite && all.q_full > 0, "thin query count", q);
  }
  if (A.general) {
    char e[100];
    std::snprintf(e, sizeof(e), "full %" PRIu64 " calls, lite %" PRIu64, all.ens_full, all.ens_lite);
    Check(all.ens_full == all.ens_lite, "EnsureContent calls", e);
  }
  Check(all.cands > 0, "the trace holds particle candidates (a check that reads none fails)");
  Check(all.taken > 0, "the lever takes draws on this trace (taken 0 = it does nothing)");
  if (fails) {
    std::printf("%d check(s) FAILED\n", fails);
    return 1;
  }
  std::puts("all checks passed");
  return 0;
}
