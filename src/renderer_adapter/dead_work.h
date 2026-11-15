// S102 lane G4 (docs\renderer\DEAD_WORK.md): the passes nothing reads, left out on the binder's side. No game or SDK includes.
//
// Two classes of pass the game records and replays every frame whose results nobody (seems to) use:
//   CardBake  the crowd card bake (sub_827FD698, pass_site 0x82C01F60): up to 20 passes of 64x64 (one draw, a clear, one copy into the type's picture).
//             The pictures are read only by the far cards (call_site 0x825130C4): a match (no far cards) bakes 15-20 passes whose pictures nothing reads.
//             The game re-bakes the pictures EVERY frame, so "not read this frame" is safe to cut (the reader of a later frame finds this frame's bake or, if
//             it is read, the bake runs in that frame).
//   PreLight  the yPreLight sss cube bake (pass 0x82C20AB8, draw call 0x82C20B90): five slot passes a frame. CENSUS ONLY (review F1): the game re-bakes a slot only
//             when its five light rows change (sub_827EBA00), so a frame with a bake and no reader followed by frames with readers and no bake would leave a stale
//             or never-baked cube. The cut is built, proven offline, and kept off until the lazy re-bake in docs\renderer\DEAD_WORK.md is built. Planted faults switch it on.
//
// The rule: a pass is DEAD in a frame when every image it writes is read by nothing in THIS frame: its copy destinations are not read by any draw's texture slot,
// any copy's source or the Present (except by another cut pass); its own targets are read by nothing outside cut passes and are the target of no kept pass; no CPU
// content is uploaded into a destination; the destination was never read back by the CPU. A dead pass is cut whole (SetTargets, Clear, Draw and Copy ops).
// Cross-frame ledger (review F2, as copy_alias's Violations): `cut_` = destinations whose last bake was cut. In every guest-frame bind a read of a cut_ image before a
// kept write counts `stale_read`, puts the image in `late_` (never cut again) and is logged by the binder; a kept write removes it from cut_. A reader BEFORE the pass
// in the same frame (read-before-write) is `late_read` and also puts the image in `late_`. The proof asserts both are 0.
// Never planned: an isolate / velocity bind, a part of a split frame (and the part after it), a lasting-only bind, a frame whose thin draws blanked their texture
// slots, a frame with no draw origins. A bake is also kept in any frame that holds a far-card draw.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capture_reader.h"
#include "copy_alias.h"

namespace rdr_adapter::dead_work {

// Live: --wwe2k14_rdr_skip_match_card_bake / --wwe2k14_rdr_skip_dead_prelight (default 0; the second is census only); fview: --dead-work MASK (1 card bake, 2 pre-light
// census, 4 census only). Written by the reader's thread every frame, read by the render thread: relaxed atomics (review F7).
inline std::atomic<bool> g_skip_card_bake{false};
inline std::atomic<bool> g_skip_prelight{false};
inline std::atomic<bool> g_census_only{false};   // plan and count, cut nothing

inline constexpr uint32_t kBakePassSite = 0x82C01F60u;
inline constexpr uint32_t kPreLightPassSite = 0x82C20AB8u;
inline constexpr uint32_t kPreLightCallSite = 0x82C20B90u;
inline constexpr uint32_t kFarCardCallSite = 0x825130C4u;

enum Class : uint8_t { kCardBake = 0, kPreLight, kClassCount };
inline const char* ClassName(int c) { return c == kCardBake ? "crowd card bake" : c == kPreLight ? "pre-light" : "?"; }

// Planted faults (tools only): the check must fail while each is on and pass with it off. Any fault also makes the pre-light cut real (it is census only otherwise).
enum class Fault : uint8_t {
  None,
  CutLive,        // the first LIVE (read) candidate is cut too: the picture check must show it
  IgnoreReaders,  // the reader scan finds nothing and the far-card guard is lifted: every candidate is cut
  StaleWindow,    // pre-light made real, its readers hidden: the baking frames are cut and the next frames read a cube nobody re-baked (stale_read >= 1, the picture differs)
  PhantomLate,    // frame 10: a synthetic reader of the first bake destination before its pass (late_read == 1, that pass is never cut again)
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "cut_live", "ignore_readers", "stale_window", "phantom_late"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}
inline constexpr uint64_t kPhantomFrame = 10;

struct Census {
  uint64_t frames = 0, frames_planned = 0, frames_unplanned = 0, frames_blind = 0;
  uint64_t cand[kClassCount] = {}, cut[kClassCount] = {}, live[kClassCount] = {}, would_cut[kClassCount] = {};   // would_cut: dead but left alone (census only)
  uint64_t ops_cut[kClassCount] = {}, draws_cut[kClassCount] = {}, copies_cut[kClassCount] = {}, targets_cut[kClassCount] = {}, clears_cut[kClassCount] = {};
  uint64_t would_copies[kClassCount] = {}, would_draws[kClassCount] = {};
  uint64_t late_read = 0;        // a reader before the pass in its frame: the destination is never cut again
  uint64_t stale_read = 0;       // a later frame read a destination whose last bake was cut before any kept write (review F2): must be 0
  uint64_t live_cut = 0;         // (planted fault only) a live pass cut
  uint64_t far_card_frames = 0;  // frames whose bake was kept because a far-card draw is in the frame
};

struct Plan {
  std::vector<uint8_t> drop;   // by op index: 1 = not recorded
  uint32_t count = 0;
  void Reset(uint32_t ops) { drop.assign(ops, 0), count = 0; }
};

// What the caller knows that the frame does not.
struct Mode {
  bool card_bake = false, prelight = false;
  std::function<const ImageRecord*(uint32_t)> record;   // an image id's record (null: unknown): the bake's 64x64 checks
  std::function<bool(const Draw&)> is_bake_draw;        // the draw's vertex program is yAudDefault/skinned_lit_lm_vertex (null: the card bake is not planned)
};

class Planner {
 public:
  Census census;
  uint32_t last_stale_image = 0;

  // Plans one frame into `plan`. `plan_ok` false: a bind that must not be planned (isolate / velocity / a part of a split frame / lasting-only): counted, nothing cut.
  // `ledger_ok` false: not a guest-frame bind at all (isolate / velocity): the cross-frame ledger is not run on it either.
  void Build(const LoadedFrame& f, const Mode& mode, bool plan_ok, bool ledger_ok, Plan& plan) {
    plan.Reset(f.ops.count);
    if (!ledger_ok) return;
    ++census.frames;
    ScanContent(f);
    newly_cut_.clear();
    PlanFrame(f, mode, plan_ok, plan);
    Ledger(f, plan);   // after the plan: a pass cut now is not a kept write; reads of earlier frames' cut destinations are judged against this frame's kept ops
    for (uint32_t d : newly_cut_) cut_.insert(d);
  }

 private:
  void PlanFrame(const LoadedFrame& f, const Mode& mode, bool plan_ok, Plan& plan) {
    if (!plan_ok) {
      ++census.frames_unplanned;
      return;
    }
    if (!f.origins.count || BlindThin(f)) {
      ++census.frames_blind;
      return;
    }
    ++census.frames_planned;
    Spans(f, mode);
    if (spans_.empty()) return;
    bool far_cards = false;
    for (uint32_t d = 0; d < f.draws.count && !far_cards; ++d) far_cards = f.OriginOf(d) && f.OriginOf(d)->call_site == kFarCardCallSite;
    if (far_cards) ++census.far_card_frames;
    const Fault fault = g_fault;
    for (Span& s : spans_) {
      if (s.cls < 0) continue;
      if (s.cls == kCardBake ? !mode.card_bake : !mode.prelight) { s.cls = -1; continue; }
      if (s.cls == kCardBake && far_cards && fault != Fault::IgnoreReaders) { s.cls = -1; continue; }   // (the planted fault also lifts this guard: it must reach the readers)
      ++census.cand[s.cls];
    }
    watch_.clear();
    for (const Span& s : spans_) {
      if (s.cls < 0) continue;
      for (uint32_t d : s.dests) watch_[d];
      for (uint32_t t : s.targets) watch_[t];
    }
    if (watch_.empty()) return;
    if (fault != Fault::IgnoreReaders) ScanReaders(f);
    if (fault == Fault::StaleWindow)   // hide the pre-light readers: the baking frames look dead
      for (const Span& s : spans_)
        if (s.cls == kPreLight)
          for (uint32_t d : s.dests) watch_[d].clear();
    phantom_ = 0;
    if (fault == Fault::PhantomLate && census.frames - 1 == kPhantomFrame)
      for (const Span& s : spans_)
        if (s.cls == kCardBake && !s.dests.empty()) { phantom_ = s.dests[0]; break; }
    // Fix point: a reader inside a dead span does not count; a pruned candidate's ops count again. Alive only ever falls, so this ends.
    for (bool changed = true; changed;) {
      changed = false;
      for (Span& s : spans_) {
        if (s.cls < 0 || !s.alive) continue;
        if (!Dead(f, s)) { s.alive = false; changed = true; }
      }
    }
    const bool real_prelight = fault != Fault::None;   // census only unless a planted fault is on
    bool cut_live_done = false;
    for (Span& s : spans_) {
      if (s.cls < 0) continue;
      bool cut = s.alive;
      if (!cut) {
        ++census.live[s.cls];
        if (fault == Fault::CutLive && !cut_live_done && s.dests_read) cut = true, cut_live_done = true, ++census.live_cut;
      }
      if (!cut) continue;
      if (s.cls == kPreLight && !real_prelight) {   // review F1: counted, not cut
        ++census.would_cut[s.cls];
        for (uint32_t p = s.begin; p < s.end; ++p) {
          if (f.ops[p].kind == OpKind::Draw) ++census.would_draws[s.cls];
          if (f.ops[p].kind == OpKind::Copy) ++census.would_copies[s.cls];
        }
        continue;
      }
      ++census.cut[s.cls];
      for (uint32_t d : s.dests) newly_cut_.push_back(d);
      for (uint32_t p = s.begin; p < s.end; ++p) {
        plan.drop[p] = 1;
        ++plan.count;
        ++census.ops_cut[s.cls];
        switch (f.ops[p].kind) {
          case OpKind::Draw: ++census.draws_cut[s.cls]; break;
          case OpKind::Copy: ++census.copies_cut[s.cls]; break;
          case OpKind::SetTargets: ++census.targets_cut[s.cls]; break;
          case OpKind::Clear: ++census.clears_cut[s.cls]; break;
          default: break;
        }
      }
    }
  }

 public:
  std::string Report(const Mode& mode) const {
    char b[640];
    std::string out;
    std::snprintf(b, sizeof b, "[rdr][dead_work] card bake %d prelight %d(census only)%s%s: frames %llu planned %llu unplanned %llu blind %llu, late reads %llu, stale reads %llu, far-card frames %llu\n",
                  mode.card_bake, mode.prelight, g_census_only ? " census-only" : "", g_fault == Fault::None ? "" : " FAULT", static_cast<unsigned long long>(census.frames),
                  static_cast<unsigned long long>(census.frames_planned), static_cast<unsigned long long>(census.frames_unplanned), static_cast<unsigned long long>(census.frames_blind),
                  static_cast<unsigned long long>(census.late_read), static_cast<unsigned long long>(census.stale_read), static_cast<unsigned long long>(census.far_card_frames));
    out += b;
    for (int c = 0; c < kClassCount; ++c) {
      std::snprintf(b, sizeof b, "[rdr][dead_work]   %-16s candidates %llu, cut %llu (targets %llu, clears %llu, draws %llu, copies %llu), would cut %llu (draws %llu, copies %llu), kept live %llu%s", ClassName(c),
                    static_cast<unsigned long long>(census.cand[c]), static_cast<unsigned long long>(census.cut[c]), static_cast<unsigned long long>(census.targets_cut[c]),
                    static_cast<unsigned long long>(census.clears_cut[c]), static_cast<unsigned long long>(census.draws_cut[c]), static_cast<unsigned long long>(census.copies_cut[c]),
                    static_cast<unsigned long long>(census.would_cut[c]), static_cast<unsigned long long>(census.would_draws[c]), static_cast<unsigned long long>(census.would_copies[c]),
                    static_cast<unsigned long long>(census.live[c]), c + 1 < kClassCount ? "\n" : "");
      out += b;
    }
    if (census.live_cut) {
      std::snprintf(b, sizeof b, "\n[rdr][dead_work]   PLANTED: %llu live passes cut", static_cast<unsigned long long>(census.live_cut));
      out += b;
    }
    return out;
  }

 private:
  struct Span {
    uint32_t begin = 0, end = 0;     // op positions [begin, end): a SetTargets up to the next one
    int cls = -1;                    // Class, -1 not a candidate
    bool alive = true;               // still a cut candidate
    bool dests_read = false;         // (fault bookkeeping) some destination is read in the frame
    std::vector<uint32_t> dests, targets;
  };
  std::vector<Span> spans_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> watch_;   // image -> positions that read it (draw slot, copy source, present)
  std::unordered_set<uint32_t> late_;                           // destinations a reader once preceded or that were read stale: never cut again
  std::unordered_set<uint32_t> cut_;                            // destinations whose last bake was cut (the cross-frame ledger)
  std::unordered_set<uint32_t> content_;
  std::vector<uint32_t> newly_cut_;
  uint32_t phantom_ = 0;

  static bool BlindThin(const LoadedFrame& f) {
    for (uint32_t d = 0; d < f.draws.count; ++d)
      if ((f.draws[d].flags & kDrawThin) && f.draws[d].texture_count == 0) return true;   // the reader blanked its slots: readers unknown
    return false;
  }

  void ScanContent(const LoadedFrame& f) {
    content_.clear();
    for (const ContentRef& c : f.contents) content_.insert(c.image);
  }

  // The cross-frame ledger (review F2): a read of a destination whose last bake was cut, before any kept write.
  void Ledger(const LoadedFrame& f, const Plan& plan) {
    if (cut_.empty()) return;
    for (uint32_t c : content_) cut_.erase(c);   // a CPU upload is a fresh write
    const auto stale = [&](uint32_t image) {
      if (!cut_.count(image)) return;
      ++census.stale_read;
      late_.insert(image);
      cut_.erase(image);
      last_stale_image = image;
    };
    for (uint32_t p = 0; p < f.ops.count && !cut_.empty(); ++p) {
      if (p < plan.drop.size() && plan.drop[p]) continue;   // not recorded: neither a read nor a write
      const Op& op = f.ops[p];
      if (op.kind == OpKind::Draw) {
        const Draw& d = f.draws[op.index];
        for (uint32_t k = 0; k < d.texture_count; ++k) stale(f.texture_slots[d.texture_first + k].image);
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& c = f.copies[op.index];
        stale(c.source.image);
        cut_.erase(c.dest.image);   // a copy into it: written (the pass that wrote it was kept)
      } else if (op.kind == OpKind::Present) {
        stale(f.presents[op.index].source.image);
      }
    }
  }

  void Spans(const LoadedFrame& f, const Mode& mode) {
    spans_.clear();
    const uint32_t n = f.ops.count;
    for (uint32_t p = 0; p < n;) {
      if (f.ops[p].kind != OpKind::SetTargets) { ++p; continue; }
      Span s;
      s.begin = p;
      uint32_t q = p + 1;
      while (q < n && f.ops[q].kind != OpKind::SetTargets) ++q;
      s.end = q;
      Classify(f, mode, s);
      spans_.push_back(std::move(s));
      p = q;
    }
  }

  static bool Covers(const rdr::Rect& r, uint32_t w, uint32_t h) { return r.x == 0 && r.y == 0 && r.width >= w && r.height >= h; }

  // A span is a candidate when it holds only Clear / Draw / Copy, at least one draw and one copy, every draw of one class by its origin, every copy reads the span's own
  // colour target. The card bake also needs (review F5) the bake's vertex program, a 64x64 colour target and 64x64 copies covering the whole destination.
  void Classify(const LoadedFrame& f, const Mode& mode, Span& s) {
    const rdr::TargetsOp& t = f.targets[f.ops[s.begin].index];
    if (!t.color_count || !t.color[0].image) return;
    const uint32_t colour = t.color[0].image;
    int cls = -1;
    uint32_t draws = 0, copies = 0;
    bool bake_shape = true;
    for (uint32_t p = s.begin + 1; p < s.end; ++p) {
      const Op& op = f.ops[p];
      switch (op.kind) {
        case OpKind::Clear: break;
        case OpKind::Draw: {
          const DrawOrigin* o = f.OriginOf(op.index);
          int c = -1;
          if (o && o->pass_site == kBakePassSite) c = kCardBake;
          else if (o && (o->pass_site == kPreLightPassSite || o->call_site == kPreLightCallSite)) c = kPreLight;
          if (c < 0 || (cls >= 0 && c != cls)) return;
          cls = c;
          if (c == kCardBake && !(mode.is_bake_draw && mode.is_bake_draw(f.draws[op.index]))) bake_shape = false;
          ++draws;
          break;
        }
        case OpKind::Copy: {
          const rdr::CopyOp& cp = f.copies[op.index];
          if (cp.source.image != colour || !cp.dest.image) return;
          const ImageRecord* rd = mode.record ? mode.record(cp.dest.image) : nullptr;
          if (!rd || rd->width != 64 || rd->height != 64 || !Covers(cp.source_rect, 64, 64) || !Covers(cp.dest_rect, 64, 64)) bake_shape = false;
          s.dests.push_back(cp.dest.image);
          ++copies;
          break;
        }
        default: return;   // a Present (or anything else) inside: not ours to cut
      }
    }
    if (!draws || !copies) return;
    if (cls == kCardBake) {
      const ImageRecord* rc = mode.record ? mode.record(colour) : nullptr;
      if (!bake_shape || !rc || rc->width != 64 || rc->height != 64) return;
    }
    s.cls = cls;
    s.targets.push_back(colour);
    if (t.depth.image) s.targets.push_back(t.depth.image);
    for (uint32_t c = 1; c < t.color_count; ++c)
      if (t.color[c].image) s.targets.push_back(t.color[c].image);
  }

  void ScanReaders(const LoadedFrame& f) {
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      const Op& op = f.ops[p];
      if (op.kind == OpKind::Draw) {
        const Draw& d = f.draws[op.index];
        for (uint32_t k = 0; k < d.texture_count; ++k) {
          const auto it = watch_.find(f.texture_slots[d.texture_first + k].image);
          if (it != watch_.end()) it->second.push_back(p);
        }
      } else if (op.kind == OpKind::Copy) {
        const auto it = watch_.find(f.copies[op.index].source.image);
        if (it != watch_.end()) it->second.push_back(p);
      } else if (op.kind == OpKind::Present) {
        const auto it = watch_.find(f.presents[op.index].source.image);
        if (it != watch_.end()) it->second.push_back(p);
      }
    }
  }

  bool InAliveSpan(uint32_t p) const {
    for (const Span& s : spans_)
      if (s.cls >= 0 && s.alive && p >= s.begin && p < s.end) return true;
    return false;
  }

  // Is the span dead given the other spans' current state? (own copy reads inside the span are of its colour target only, never a destination.)
  bool Dead(const LoadedFrame& f, Span& s) {
    bool dead = true;
    for (uint32_t d : s.dests) {
      if (content_.count(d) || late_.count(d) || copy_alias::WasReadBack(d)) dead = false;   // (review F10: a CPU read-back of the picture)
      if (phantom_ && d == phantom_) {   // planted: a reader before the pass
        s.dests_read = true;
        dead = false;
        if (!late_.count(d)) { late_.insert(d); ++census.late_read; }
      }
      const auto it = watch_.find(d);
      if (it == watch_.end()) continue;
      for (uint32_t pos : it->second) {
        if (pos >= s.begin && pos < s.end) continue;
        if (InAliveSpan(pos)) continue;   // read by another dead pass: that pass is not recorded
        s.dests_read = true;
        dead = false;
        if (pos < s.begin && !late_.count(d)) {   // the reader precedes the pass: it read the previous frame's bake
          late_.insert(d);
          ++census.late_read;
        }
      }
    }
    for (uint32_t t : s.targets) {   // own targets: read by nothing outside dead passes
      const auto it = watch_.find(t);
      if (it != watch_.end())
        for (uint32_t pos : it->second)
          if (!(pos >= s.begin && pos < s.end) && !InAliveSpan(pos)) dead = false;
      for (const Span& o : spans_) {   // and the target of no other (kept) pass: its content would carry over
        if (&o == &s) continue;
        if (o.cls >= 0 && o.alive) continue;
        const rdr::TargetsOp& ot = f.targets[f.ops[o.begin].index];
        for (uint32_t c = 0; c < ot.color_count; ++c)
          if (ot.color[c].image == t) dead = false;
        if (ot.depth.image == t) dead = false;
      }
    }
    return dead;
  }
};

}  // namespace rdr_adapter::dead_work
