// load_order_unit: lane S105 SKB's offline proof (0 game runs, no game files): PickJob (src\renderer_scene\load_order.h), the live loader's pick.
//   load_order_unit [--plant fifo|nobulk|gate_ambient|gate_midmatch]     each line PASS / FAIL, exit 1 on any FAIL. Twin fifo: the old strict-FIFO pick (pop front) must FAIL gates_first.
// entry: PickJob (the pure pick LoaderLoop calls with the queue lock held; Drain mirrors its pick + erase)
// Run 110's order: the arena, 14 non-gating m2 props, then the two wrestlers that gate READY.
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "../../src/renderer_scene/load_order.h"

namespace {
struct M { std::string key; bool gates = true; bool bulk = false; };
struct Job { M m; int arena = -1; };
int g_fail = 0;
void Check(const char* name, bool ok, const char* detail) { std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail); g_fail += !ok; }
bool bulk_alone = false, fifo = false, nobulk = false, gate_all = false, gate_mid = false;
// twin nobulk: the S105 SKB first rule (gating first, else the oldest), blind to `bulk`
size_t PickNoBulk(const std::deque<Job>& q) { size_t i = 0; for (const Job& j : q) { if (j.arena >= 0 || j.m.gates) return i; ++i; } return 0; }
size_t Pick(const std::deque<Job>& q, bool allow_bulk = true) { return fifo ? 0 : nobulk ? PickNoBulk(q) : bulk_alone ? rdr_scene::PickJob(q, true) : rdr_scene::PickJob(q, allow_bulk); }
// drain like LoaderLoop: pick, erase, record the key
std::string Drain(std::deque<Job> q) {
  std::string order;
  while (!q.empty()) { const size_t at = Pick(q); order += (q[at].arena >= 0 ? "arena" : q[at].m.key) + " "; q.erase(q.begin() + std::ptrdiff_t(at)); }
  return order;
}
Job Prop(const char* k) { Job j; j.m.key = k; j.m.gates = false; return j; }
Job Gate(const char* k) { Job j; j.m.key = k; j.m.gates = true; return j; }
Job Arena() { Job j; j.arena = 7; j.m.gates = false; return j; }   // F9: the .arena term alone must put it first
Job Bulk(const char* k) { Job j; j.m.key = k; j.m.gates = false; j.m.bulk = true; return j; }
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) { const char* p = argv[++i]; fifo = !std::strcmp(p, "fifo"); bulk_alone = !std::strcmp(p, "bulk_alone"); nobulk = !std::strcmp(p, "nobulk"); gate_all = !std::strcmp(p, "gate_ambient"); gate_mid = !std::strcmp(p, "gate_midmatch"); }
  {   // run 110: arena, 14 props, 2 wrestlers -> arena, w1, w2, then the props in their order
    std::deque<Job> q;
    q.push_back(Arena());
    for (int i = 0; i < 14; ++i) q.push_back(Prop(("p" + std::to_string(i)).c_str()));
    q.push_back(Gate("w1")), q.push_back(Gate("w2"));
    const std::string o = Drain(q);
    Check("gates_first", o.rfind("arena w1 w2 p0 p1 ", 0) == 0, o.c_str());
    Check("props_keep_fifo_and_all_run", o.find("p13 ") != std::string::npos && o.find("p0 ") < o.find("p13 "), "every prop loads, in queue order");
  }
  {   // FIFO among equals: three gating jobs and a prop between them
    std::deque<Job> q;
    q.push_back(Gate("a")), q.push_back(Prop("x")), q.push_back(Gate("b")), q.push_back(Gate("c"));
    const std::string o = Drain(q);
    Check("fifo_among_equals", o == "a b c x ", o.c_str());
  }
  {   // all non-gating: plain FIFO; a single job
    std::deque<Job> q;
    q.push_back(Prop("x")), q.push_back(Prop("y"));
    const std::string o = Drain(q);
    Check("nongating_only_is_fifo", o == "x y ", o.c_str());
    std::deque<Job> one;
    one.push_back(Prop("z"));
    Check("single_job", rdr_scene::PickJob(one) == 0, "index 0");
  }
  {   // no starvation: gating jobs are a finite set; a non-gating job runs as soon as none is queued
    std::deque<Job> q;
    q.push_back(Prop("x")), q.push_back(Gate("g1"));
    std::string order;
    size_t at = Pick(q);
    order += q[at].m.key + " ";
    q.erase(q.begin() + std::ptrdiff_t(at));
    at = Pick(q);   // only the prop remains
    order += q[at].m.key + " ";
    Check("no_starvation_once_gates_done", order == "g1 x ", order.c_str());
  }
  {   // S105 SKB fix F3: the evtobj prewarm is bulk: it runs after every other job, even ones queued after it; bulk-only queues are FIFO
    std::deque<Job> q;
    q.push_back(Arena()), q.push_back(Bulk("pre")), q.push_back(Prop("x")), q.push_back(Gate("w")), q.push_back(Prop("y"));
    const std::string o = Drain(q);
    Check("bulk_runs_last", o == "arena w x y pre ", o.c_str());
    std::deque<Job> bq;
    bq.push_back(Bulk("b1")), bq.push_back(Bulk("b2"));
    Check("bulk_only_is_fifo", Drain(bq) == "b1 b2 ", "b1 b2");
    std::deque<Job> late;   // a gating ask that arrives while the prewarm is still queued is picked first (the entrant's event prop)
    late.push_back(Bulk("pre")), late.push_back(Gate("evtprop"));
    Check("gating_overtakes_queued_bulk", Drain(late) == "evtprop pre ", Drain(late).c_str());
  }
  {   // S105 SKB2: the prewarm never starts before the roster is in (allow_bulk false); a gating job asked while it is pending is picked first; twin bulk_alone: the old pick starts a lone bulk job at once
    std::deque<Job> q;
    q.push_back(Arena()), q.push_back(Bulk("pre"));
    size_t at = Pick(q, false);
    q.erase(q.begin() + std::ptrdiff_t(at));   // the arena
    Check("bulk_held_until_roster", Pick(q, false) == rdr_scene::kNoJob, "only the prewarm queued, roster not in: nothing starts");
    q.push_back(Gate("w1"));
    Check("gate_asked_while_prewarm_pending_first", q[Pick(q, false)].m.key == "w1", "the wrestler asked after the prewarm queued runs first");
    q.erase(q.begin() + 1);
    Check("bulk_starts_once_roster_in", q[Pick(q, true)].m.key == "pre", "allow_bulk true: the prewarm runs");
    std::deque<Job> mix;
    mix.push_back(Bulk("pre")), mix.push_back(Prop("x"));
    Check("plain_runs_while_bulk_held", mix[Pick(mix, false)].m.key == "x", "a plain job is not held by the bulk hold");
  }
  {   // S105 SKB fix F3: the entrant's event props and the match structure gate READY; ambient props (m2 weapons, gm sets, blood) do not. Twin gate_ambient: the m2 weapons gate too.
    using rdr_scene::PropAsk;
    const auto g = [&](PropAsk k, bool open = true) { return gate_all ? true : rdr_scene::PropAskGatesReady(k, gate_mid ? true : open); };
    Check("entrant_props_gate", g(PropAsk::EventProp) && g(PropAsk::StageStructure), "event props and the match structure hold READY");
    Check("midmatch_asks_do_not_gate", !g(PropAsk::EventProp, false) && !g(PropAsk::StageStructure, false), "once the scene has drawn, event / structure props pop in (twin gate_midmatch: they gate)");
    Check("ambient_props_do_not_gate", !g(PropAsk::M2Weapon) && !g(PropAsk::GmSet) && !g(PropAsk::Blood), "m2 weapons, gm sets, blood stay non-gating");
  }
  {   // S105 SKB fix F3: the prewarm is released once at the match phase when no event prop is wanted or held; an entrance (phase 4) or a wanted prop never releases; re-armed when the phase leaves 1
    bool latch = false, pend = false;
    rdr_scene::BellRelease(4, true, true, latch, pend);
    Check("bell_no_release_in_entrance", !pend, "phase 4: nothing");
    rdr_scene::BellRelease(1, false, true, latch, pend);
    Check("bell_no_release_while_prop_wanted", !pend, "phase 1 with a wanted event prop: nothing");
    rdr_scene::BellRelease(1, true, true, latch, pend);
    Check("bell_releases_once", pend && latch, "phase 1, no prop: release pending");
    pend = false;
    rdr_scene::BellRelease(1, true, true, latch, pend);
    Check("bell_release_is_once_per_stretch", !pend, "no second release in the same phase-1 stretch");
    rdr_scene::BellRelease(4, true, true, latch, pend), rdr_scene::BellRelease(1, true, true, latch, pend);
    Check("bell_rearms_after_entrance", pend, "phase 4 then 1 releases again");
  }
  std::printf("%s\n", g_fail ? "load_order_unit: FAIL" : "load_order_unit: PASS");
  return g_fail ? 1 : 0;
}
