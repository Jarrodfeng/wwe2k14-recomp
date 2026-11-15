// S102 lane P4P5 (docs\renderer\OWN_SHADOW_RIM.md): the shadow module's and the rim map's draw nodes as GROUPS of a replayed frame. GPU-free, no game or SDK includes.
//
// The game's frame has one walk (R4 1.1); the shadow module (sub_827E9740 -> sub_827E95D8) and the rim map (sub_827EA7B8 -> sub_827EA098) are two of its draw nodes (category 1). Each
// one's walk leaves, in the recorded frame, a run of whole passes (a SetTargets up to the next SetTargets, with its clears, draws and copies). This header answers which passes are
// whose, from what the stream names (the draws' origins: the recorder's pass site and queue site, the pixel program) and never from a shader hash alone, and lays the group out:
// the first / last pass, the ops, and whether the group is ONE run of the frame's ops (nothing foreign inside). The binder (marked S102 P4P5 lines) takes a group out of the walk
// and puts it back as one unit at the first pass's place (the order is the stream's: a group that is one run needs no move), counts it, and for the test arm `none` leaves it out.
//
// What this is NOT: the live decision. The node switch (node_switch.h) is decided in the game on the record thread; this is the offline step that proves the node's passes are a clean unit
// whose removal is the emulation of "the node returned at once" (tools\shadow\own_nodes_cut.py is the same rule in Python; the gate compares the two).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rdr_adapter::own_group {

enum Group : uint8_t { kNone = 0, kModule = 1, kRim = 2, kGroupCount = 3 };
inline const char* GroupName(uint8_t g) { return g == kModule ? "shadow_module" : g == kRim ? "rim_map" : "none"; }

// Switches (fview / rplay --own-shadow-module[=none], --own-rim-map[=none]; the live switches are wwe2k14_rdr_own_shadow_module / _own_rim_map in the node hook).
inline std::atomic<bool> g_on[kGroupCount];      // the group is taken out of the walk and re-issued as one unit (default off)
inline std::atomic<bool> g_none[kGroupCount];    // test arm: taken out and NOT re-issued ("cutting the node without drawing ours": must fail the picture check)

// PLANTED FAULTS (fview --group-fault NAME; tests only; each must fail its named check in tools\shadow\own_nodes_check.py).
//   no_reduce  the coverage reductions (call 0x82C1F514) are not counted as the module's: the group the planner finds is smaller than the node's walk (the census count check fails)
//   overlap    the first op of every group is treated as another planner's drop (G4 dead work): the group must be declined and counted, not taken
//   take_plane the plane chain (category 0, lane P3) is counted as the module's: the group takes passes that are not this node's (the census count check fails)
inline std::atomic<uint32_t> g_fault{0};
enum Fault : uint32_t { kFaultNone = 0, kFaultNoReduce = 1, kFaultTakePlane = 2, kFaultOverlap = 3 };

// The recorder's sites (guest addresses), the same constants as tools\shadow\shadow_census.py [C: SHADOW_PROMOTION_MAP.md 1.1, RIM_MAP_OURS.md].
inline constexpr uint32_t kMapPassSite = 0x82C04634u;     // the part draw inside T_ShadowMap: props map, maps A / B
inline constexpr uint32_t kPlanePassSite = 0x82BF4D38u;   // inside T_PlaneShadowMap (category 0, not this lane's)
inline constexpr uint32_t kRimPassSite = 0x82BFBD58u;     // inside T_RimLightMap
inline constexpr uint32_t kCircleCall = 0x828131E4u;      // the circle blobs (inside the props map pass)
inline constexpr uint32_t kReduceCall = 0x82C1F514u;      // the module's two coverage reductions
inline constexpr uint32_t kRimReduceCall = 0x82C1F48Cu;   // the rim map's reductions (yBlur flag_any); the same site as other post quads
inline constexpr uint32_t kMinBlurCall = 0x82814080u;     // the module's min blurs AND the rim's blur (yBlur t2): told apart by the pixel program
inline constexpr uint32_t kPlaneBlurCall = 0x827EEC84u, kPlaneDownCall = 0x828159FCu;

// One pass of a frame as the binder saw it (the planner needs nothing else).
struct PassFacts {
  uint32_t first_op = 0, end_op = 0;   // the ops [first_op, end_op) of the pass: its SetTargets up to the next
  uint32_t draws = 0, copies = 0, clears = 0;
  bool map_site = false, plane_site = false, rim_site = false, circle_call = false, reduce_call = false, plane_chain_call = false;
  bool rim_reduce_call = false, rim_reduce_program = false;   // call 0x82C1F48C / pixel program yBlur flag_any
  bool min_blur_call = false, min_blur_t2 = false, min_blur_min = false;   // call 0x82814080 / pixel program yBlur t2 / yBlur min_*
};

// shadow_census.py's rule, in its order.
inline Group Classify(const PassFacts& p) {
  if (p.draws == 0) return kNone;
  const uint32_t fault = g_fault.load(std::memory_order_relaxed);
  if (p.plane_site) return fault == kFaultTakePlane ? kModule : kNone;
  if (p.map_site) return kModule;
  if (p.rim_site) return kRim;
  if (p.plane_chain_call) return fault == kFaultTakePlane ? kModule : kNone;
  if (p.reduce_call) return fault == kFaultNoReduce ? kNone : kModule;
  if (p.rim_reduce_call && p.rim_reduce_program) return kRim;
  if (p.min_blur_call) return p.min_blur_t2 ? kRim : p.min_blur_min ? kModule : kNone;
  if (p.circle_call) return kModule;
  return kNone;
}

// A draw as the facts builder needs it: its origin words and which pixel program it has (the binder fills `ps_*` from the variant name).
struct DrawFacts {
  bool has_origin = false;
  uint8_t op = 0;                     // DrawOrigin::op (68 = a render-thread callback)
  uint32_t pass_site = 0, call_site = 0, part = 0;
  bool ps_flag_any = false, ps_t2 = false, ps_min = false;   // yBlur flag_any / t2 / min_*
};
inline constexpr uint32_t kGenericQueue3 = 0x82C1F674u;
inline uint32_t QueueSite(const DrawFacts& d) {   // pcap.queue_site: the generic queue site rides in `part` for an op-68 record
  const bool generic = d.op == 68 && (d.part == kRimReduceCall || d.part == kReduceCall || d.part == kGenericQueue3);
  return generic ? d.part : d.call_site;
}

// The rule-independent oracle (review item 7): a draw whose ORIGIN is the group's, whatever pass it sits in. The shadow module: the map pass site, the circle call, the coverage reductions, a min blur
// that is not the rim's t2. The rim map: its pass site, and its t2 blur. (The rim's flag_any reductions share a queue site with other post quads and are left to the pass rule.)
inline bool OriginIsModule(const DrawFacts& d) {
  if (!d.has_origin) return false;
  const uint32_t site = QueueSite(d);
  return d.pass_site == kMapPassSite || site == kCircleCall || site == kReduceCall || (site == kMinBlurCall && !d.ps_t2 && d.ps_min);
}
inline bool OriginIsRim(const DrawFacts& d) {
  if (!d.has_origin) return false;
  return d.pass_site == kRimPassSite || (QueueSite(d) == kMinBlurCall && d.ps_t2);
}

// Builds PassFacts from the ops of a frame, in order. A Present closes the open pass and ends it: the ops after it are no pass's (review item 6: a pass never swallows the present).
class FactsBuilder {
 public:
  void SetTargets(uint32_t op_index) {
    Close(op_index);
    cur_ = PassFacts{};
    cur_.first_op = op_index;
    open_ = true;
  }
  void Clear() { if (open_) ++cur_.clears; }
  void Copy() { if (open_) ++cur_.copies; }
  void Present(uint32_t op_index) { Close(op_index); }
  void Draw(const DrawFacts& d) {
    if (!open_) return;
    ++cur_.draws;
    if (!d.has_origin) return;
    const uint32_t site = QueueSite(d);
    cur_.map_site |= d.pass_site == kMapPassSite;
    cur_.plane_site |= d.pass_site == kPlanePassSite;
    cur_.rim_site |= d.pass_site == kRimPassSite;
    cur_.circle_call |= site == kCircleCall;
    cur_.reduce_call |= site == kReduceCall;
    cur_.plane_chain_call |= site == kPlaneBlurCall || site == kPlaneDownCall;
    if (site == kRimReduceCall) {
      cur_.rim_reduce_call = true;
      cur_.rim_reduce_program |= d.ps_flag_any;
    }
    if (site == kMinBlurCall) {
      cur_.min_blur_call = true;
      cur_.min_blur_t2 |= d.ps_t2;
      cur_.min_blur_min |= d.ps_min;
    }
  }
  const std::vector<PassFacts>& Finish(uint32_t op_count) {
    Close(op_count);
    return passes_;
  }
  void Reset() { passes_.clear(), open_ = false; }
 private:
  void Close(uint32_t end) {
    if (!open_) return;
    cur_.end_op = end;
    passes_.push_back(cur_);
    open_ = false;
  }
  std::vector<PassFacts> passes_;
  PassFacts cur_;
  bool open_ = false;
};

// Start-time refusal (review item 6): the D3 caster skip drops the same caster passes at another place; two cuts of one pass. nullptr = fine.
inline const char* Refusal(bool caster_d3, bool shadow_module_on, bool rim_map_on) {
  if (caster_d3 && (shadow_module_on || rim_map_on))
    return "refused: the D3 caster skip (wwe2k14_rdr_caster_d3) and wwe2k14_rdr_own_shadow_module / own_rim_map are both on: two cuts of the same caster passes; the game walks the nodes";
  return nullptr;
}

struct GroupFacts {
  uint32_t passes = 0, ops = 0, draws = 0, copies = 0;
  uint32_t first_op = 0, last_op = 0;   // [first_op, last_op): the span of the frame's ops from the group's first pass to the end of its last
  uint32_t foreign_passes = 0;          // passes of the frame inside that span that are not the group's (0 = the group is one run)
  bool Present() const { return passes != 0; }
  bool OneRun() const { return passes != 0 && foreign_passes == 0 && ops == last_op - first_op; }
};

struct Plan {
  GroupFacts g[kGroupCount];
  bool taken[kGroupCount] = {};          // the group is a unit this frame (switch on, one run)
  std::vector<uint8_t> group_of_op;      // per op: Group, kNone for the rest (filled only when a switch is on)
};

// Lays the groups out. `passes` in op order (they partition the frame's ops).
inline void Layout(const std::vector<PassFacts>& passes, uint32_t op_count, Plan& plan) {
  plan = Plan{};
  plan.group_of_op.assign(op_count, kNone);
  std::vector<uint8_t> kind(passes.size(), kNone);
  for (size_t i = 0; i < passes.size(); ++i) {
    kind[i] = Classify(passes[i]);
    const uint8_t k = kind[i];
    if (k == kNone) continue;
    GroupFacts& gf = plan.g[k];
    if (gf.passes == 0) gf.first_op = passes[i].first_op;
    gf.last_op = passes[i].end_op;
    ++gf.passes;
    gf.ops += passes[i].end_op - passes[i].first_op;
    gf.draws += passes[i].draws;
    gf.copies += passes[i].copies;
    for (uint32_t o = passes[i].first_op; o < passes[i].end_op && o < op_count; ++o) plan.group_of_op[o] = k;
  }
  for (size_t i = 0; i < passes.size(); ++i)
    for (uint8_t k = kModule; k < kGroupCount; ++k)
      if (plan.g[k].Present() && kind[i] != k && passes[i].first_op >= plan.g[k].first_op && passes[i].first_op < plan.g[k].last_op) ++plan.g[k].foreign_passes;
}

struct Counters {
  uint64_t frames[kGroupCount] = {};          // frames that carried the group
  uint64_t frames_taken[kGroupCount] = {};    // frames the group was taken as a unit
  uint64_t frames_not_one_run[kGroupCount] = {};   // frames the group had a foreign pass inside: left as the stream has it
  uint64_t passes[kGroupCount] = {}, ops[kGroupCount] = {}, draws[kGroupCount] = {}, copies[kGroupCount] = {};
  uint64_t ops_left_out[kGroupCount] = {};    // the `none` arm: stream ops not recorded
  uint64_t draws_left_out[kGroupCount] = {}, copies_left_out[kGroupCount] = {};
  uint64_t reissued[kGroupCount] = {};        // re-issued as one unit at the first pass's place (ops)
  uint64_t declined_overlap[kGroupCount] = {};   // frames the group was declined: an op of it is in another planner's drop set (G4 dead work, P1 bake, particle runs)
  uint64_t oracle_left[kGroupCount] = {};     // the `none` arm: draws NOT left out whose origin is still the group's (must be 0)
};

inline std::string CensusLine(const Counters& c) {
  char line[1200];
  std::snprintf(line, sizeof line,
                "[rdr][own_group] shadow_module: frames %llu, taken %llu, not one run %llu, passes %llu, ops %llu, draws %llu, copies %llu, re-issued ops %llu, left out ops %llu (draws %llu, copies %llu), declined %llu, oracle left %llu; "
                "rim_map: frames %llu, taken %llu, not one run %llu, passes %llu, ops %llu, draws %llu, copies %llu, re-issued ops %llu, left out ops %llu (draws %llu, copies %llu), declined %llu, oracle left %llu",
                (unsigned long long)c.frames[kModule], (unsigned long long)c.frames_taken[kModule], (unsigned long long)c.frames_not_one_run[kModule], (unsigned long long)c.passes[kModule],
                (unsigned long long)c.ops[kModule], (unsigned long long)c.draws[kModule], (unsigned long long)c.copies[kModule], (unsigned long long)c.reissued[kModule],
                (unsigned long long)c.ops_left_out[kModule], (unsigned long long)c.draws_left_out[kModule], (unsigned long long)c.copies_left_out[kModule], (unsigned long long)c.declined_overlap[kModule], (unsigned long long)c.oracle_left[kModule], (unsigned long long)c.frames[kRim],
                (unsigned long long)c.frames_taken[kRim], (unsigned long long)c.frames_not_one_run[kRim], (unsigned long long)c.passes[kRim], (unsigned long long)c.ops[kRim],
                (unsigned long long)c.draws[kRim], (unsigned long long)c.copies[kRim], (unsigned long long)c.reissued[kRim], (unsigned long long)c.ops_left_out[kRim],
                (unsigned long long)c.draws_left_out[kRim], (unsigned long long)c.copies_left_out[kRim], (unsigned long long)c.declined_overlap[kRim], (unsigned long long)c.oracle_left[kRim]);
  return line;
}

}  // namespace rdr_adapter::own_group
