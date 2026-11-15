// S102 lane R24-F (docs\renderer\GPU_STAMPS.md "Group stamps"): GPU timestamps at the change of draw GROUP inside a colour / frame pass. Pure, no Vulkan, no game: the
// group ids, the segment planner the recorder runs per draw, the chain check and the per-frame aggregation, so tools\r24f\group_stamps_test.cpp runs the REAL code.
//
// The frame carries one group id per draw (Frame::draw_groups, written by the binder from the draw's origin and fate, draw_groups.h) and the min run
// (Frame::group_min_run, 0 = off). RecordPass asks the planner before each draw it issues; on "yes" it writes ONE timestamp (BOTTOM_OF_PIPE, inside the render pass,
// legal on a graphics queue) into the slot's own group pool. A pass is then a CHAIN of segments: begin (the previous step's stamp) -> group stamp 1 -> ... -> the
// pass's own closing stamp. The chain's spans add up to the pass's own span by construction; the collector checks the chain is continuous and that the draws counted
// into the segments equal the draws the pass issued (the two checks the planted twins break).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rdr {

// --wwe2k14_rdr_gpu_group_stamps (PerFrame, default 0): 0 = off; N >= 1 = a boundary is stamped only when the segment before it holds at least N draws (1 = every change
// of group). rplay / fview: --gpu-group-stamps N. The frame carries the value (Frame::group_min_run); the render thread reads only that.
inline std::atomic<int> g_gpu_group_stamps{0};
// TEST ONLY (rplay --gpu-group-plant N): 1 = a stamp is written but its segment is not recorded (the chain breaks), 2 = a draw is left out of the segment counts (coverage < 100 %),
// 3 = the group of the first draw after every boundary is forgotten (the segment is labelled by a group that is not its own: the purity line moves).
inline std::atomic<int> g_gpu_group_plant{0};

enum GroupId : uint8_t {
  kGrpUnnamed = 0,   // no origin reached the draw
  kGrpModelColour,   // a model part at the colour site (arena / wrestler / props the hooks did not name: the stream's own draw)
  kGrpModelMirror,   // a model part at the mirror-pass site
  kGrpModelOther,    // a model part at any other pass site
  kGrpArena,         // drawn by our arena hook
  kGrpWrestler,      // our wrestler hook (and Create a Superstar's)
  kGrpRing,          // our ring hook
  kGrpProps,         // our props hook
  kGrpCrowd,         // the crowd's members (stream or ours)
  kGrpCrowdCards,    // the far flat crowd cards
  kGrpCrowdFloor,    // the seated floor people
  kGrpParticle,
  kGrpSprite,
  kGrpText,
  kGrpMovie,
  kGrpPost,
  kGrpShadow,        // a shadow-map pass draw (casters)
  kGrpPlaneShadow,   // the ring-plane shadow chain (the shadow on the receiver)
  kGrpCircleShadow,  // the blob shadows
  kGrpFlash,
  kGrpScreens,       // the stage screens
  kGrpClear,
  kGrpRim,
  kGrpOther,         // named, none of the above (or a draw an owned group / hook emitted without an origin of its own)
  kGrpIdle,          // a segment with no draw in it (the pass's clears / loads / stores and whatever the first stamp waits for)
  kGrpCount
};
static_assert(kGrpCount <= 63, "the table key holds the group in 6 bits");

inline const char* GroupName(uint32_t g) {
  static const char* const k[] = {"unnamed", "model_colour", "model_mirror", "model_other", "arena", "wrestler", "ring", "props", "crowd", "crowd_cards", "crowd_floor", "particle",
                                  "sprite", "text", "movie", "post", "shadow", "plane_shadow", "circle_shadow", "flash", "screens", "clear", "rim", "other", "no_draws"};
  static_assert(sizeof(k) / sizeof(k[0]) == kGrpCount, "GroupName: one name per GroupId");
  return g < kGrpCount ? k[g] : "?";
}

inline constexpr uint32_t kGroupPoolCap = 384;   // group-stamp queries a slot (a frame past it stops cutting and counts an overflow: the rest of its pass is one mixed segment)

// One segment of a pass: the span between two stamps. `begin` / `end` name a stamp: bit 31 set = the slot's group pool, else the main pool.
struct GroupSeg {
  uint32_t begin = 0, end = 0;
  uint16_t pass_end_stamp = 0;   // the main-pool stamp that closes the pass step (identifies the pass in the frame)
  uint8_t dominant = 0;          // the group with the most draws in the segment (earliest on a tie)
  uint8_t pad = 0;
  uint16_t counts[kGrpCount] = {};   // draws per group in the segment
};
inline constexpr uint32_t kPoolBit = 0x80000000u;

// The recorder's per-pass decision. Add every ISSUED draw in order; NeedStamp(g) is asked BEFORE issuing the draw of group g.
struct SegmentPlanner {
  uint32_t min_run = 1;        // 0 behaves as 1
  uint32_t budget = 0;         // stamps this pass may still write
  uint32_t draws = 0;          // draws in the open segment
  uint8_t last = 0;            // group of the last draw added
  uint16_t counts[kGrpCount] = {};
  bool NeedStamp(uint8_t g) const { return draws > 0 && g != last && draws >= std::max<uint32_t>(1, min_run) && budget > 0; }
  void Add(uint8_t g) { ++counts[g < kGrpCount ? g : kGrpOther], ++draws, last = g; }
  void Close(GroupSeg& seg) {   // fills dominant + counts, starts the next segment's counts empty (the draw that follows goes into the next segment)
    uint16_t best = 0;
    uint8_t dom = last;
    for (uint8_t i = 0; i < kGrpCount; ++i)
      if (counts[i] > best) best = counts[i], dom = i;
    seg.dominant = draws ? dom : uint8_t(kGrpIdle);
    std::memcpy(seg.counts, counts, sizeof counts);
    std::memset(counts, 0, sizeof counts);
    draws = 0;
  }
};

// The collector's check over one pass's chain: segments in order; span(begin, end) gives a segment's ns (the caller masks the tick wrap). Returns the sum of the spans; `broken`
// counts a segment whose begin is not the previous segment's end (a segment missing from the chain), whose span is wrapped / above `limit_ns`, or a chain that does not end at `last_end`.
struct ChainResult {
  double sum_ns = 0;
  uint32_t broken = 0;
};
template <class SpanOf>
inline ChainResult CheckChain(const GroupSeg* segs, uint32_t n, uint32_t first_begin, uint32_t last_end, double limit_ns, SpanOf span) {
  ChainResult r;
  uint32_t expect = first_begin;
  for (uint32_t i = 0; i < n; ++i) {
    if (segs[i].begin != expect) ++r.broken;
    const double ns = span(segs[i].begin, segs[i].end);
    if (!(ns >= 0) || ns > limit_ns) ++r.broken;
    else r.sum_ns += ns;
    expect = segs[i].end;
  }
  if (expect != last_end) ++r.broken;
  return r;
}

// Per-frame aggregation of one pass: ns per group (segments are attributed to their dominant group), draws per group (exact, from the counts), and the draws that sit in
// a segment labelled with another group (the purity numbers: 0 when the min run is 1).
struct PassGroups {
  double ns[kGrpCount] = {};
  uint32_t draws[kGrpCount] = {};
  uint32_t mixed_draws = 0, total_draws = 0;
};
inline void Aggregate(const GroupSeg& s, double span_ns, PassGroups& out) {
  out.ns[s.dominant < kGrpCount ? s.dominant : kGrpOther] += span_ns;
  for (uint8_t g = 0; g < kGrpCount; ++g) {
    out.draws[g] += s.counts[g];
    out.total_draws += s.counts[g];
    if (g != s.dominant) out.mixed_draws += s.counts[g];
  }
}

}  // namespace rdr
