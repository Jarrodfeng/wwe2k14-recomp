// S102 lane R18-B1 (docs\renderer\FRAME_ROUNDTRIP.md): the frame buffer's round trip, step 1: IDENTITY RESTORES. No game or SDK includes.
//
// The 360 resolves its eDRAM scene to textures (copies of the scene targets 1194 / 1195 into 2 / 5), draws the textures back into the next surface (the colour + depth RESTORE, ps 9A836B68E3F8F0B0)
// and, later in the frame, snapshots the frame buffer again (copy of 8 into 2: the colour of the frame buffer), draws that texture back into the target it is a whole copy of (the SELF BLIT, G11) and
// restores a second time. A restore whose colour source is a whole copy of its target AND whose depth source holds exactly the depth the target holds writes pixels the target already has: the frame
// proves it is the identity and the draw is left out (fate `frame_identity`). This is G11's blit rule (provenance + write versions) carried over to the restore.
//
// The proof is the frame's own ops, never guest memory:
//   - EQUAL PAIRS: two images that hold the same bits, each with the write version it had when the equality was made. Made by (a) a whole image copy the walk really issued (same size, format, scaling,
//     mip 0: G11's `whole`), (b) a restore that passed G11's static rules (colour dst == colour src, depth dst == depth src, whether the walk draws it, turns it into copies or leaves it out). A write to
//     either image makes the pair stale (the version moves), so every later question about it says "written since".
//   - WRITE VERSIONS (per image, one per op that can change it): a clear, a copy into it, a draw that has it as a colour target; a draw writes a DEPTH target only when its state can (depth test AND depth
//     write on, or stencil test on: a stencil op can write): a draw with depth write off cannot move the depth. The hidden draws of the frame (11 model_part, 24 flares: 0 pixels on 89 of 89 Bryan frames,
//     measured with an occlusion query offline) are NOT trusted as non-writers: they count as writers by their state, nothing else. A planner that cannot prove them harmless declines (the decline reason
//     says "written since").
//   - A draw this lane leaves out writes nothing (CancelDraw): the identity blit and the identity restore do not move a version, so the pair they rely on stays valid for the next one.
// Declines (counted by reason): unusable frame (isolate / velocity / persist-only / split, DLSS), G11's static rules (shaders, state, rectangle, sizes, stencil, aliases), no equal pair for the colour,
// the colour pair written since, no equal pair for the depth, the depth pair written since.
//
// Step 2 (one image for the span: scene target and frame buffer aliased) is NOT in this file yet: the gate lane's notes say why.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame_cuts.h"

namespace rdr_adapter::frame_roundtrip {

// fview's / rplay's fallback for the switch (the live table is not running there): --frame-roundtrip N. 0 off, 1 step 1 (identity restores and the blit's identity), 2 step 1 + step 2.
inline std::atomic<int> g_fview_level{0};
inline std::atomic<bool> g_trace{false};   // --frame-roundtrip-trace: one line per decision (offline only)

// Planted faults (tools only; each must change a picture, a count or a unit-test verdict).
enum class Fault : uint8_t {
  None,
  IdentityAfterWrite,  // the version comparison of an equal pair is skipped: a pair written since still counts (unit test; fview where a write sits between)
  HiddenAsProof,       // a draw writes nothing, as if the 0-pixel hidden draws were proven non-writers (unit test)
  WrongDepth,          // the depth pair is not asked: a restore whose colour is the identity is left out whatever the depth holds
  NoPair,              // no pair is asked at all: every restore that passes G11's static rules is left out (the pictures must differ)
  KeepClear,           // span: the snapshots and the restore are left out but the clear of the scene target stays (the scene is wiped: the pictures must differ)
  NoLedger,            // review F2: the ledger never reports a read of a snapshot the span stopped writing (unit test; the two-frame case must then FAIL)
  KeepDepthClear,      // span: a DEPTH-only clear stays where the clear was dropped (the depth the restore would have put back is wiped: the pictures must differ)
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "identity_after_write", "hidden_as_proof", "wrong_depth", "no_pair", "keep_clear", "no_ledger", "keep_depth_clear"};
  static_assert(sizeof(k) / sizeof(k[0]) == size_t(Fault::Count), "a fault needs its name");
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}

enum class Why : uint8_t { Taken, Unusable, Static, NoColourPair, ColourStale, NoDepthPair, DepthStale, Count };
inline const char* WhyName(int w) {
  static const char* const k[] = {"taken", "unusable frame", "G11's static rules", "no equal colour pair", "colour pair written since", "no equal depth pair", "depth pair written since", "?"};
  return w >= 0 && w < int(Why::Count) ? k[w] : "?";
}

// Step 2, the scene target's round trip (a SPAN): the snapshot copies of the frame buffer (F -> X / Y), the clear of the scene target S, the restore of X / Y into S. When S still holds exactly what the snapshots would
// hold (a pair F == S made by the first restore, S written by nothing since), clear + restore put S back to itself: all four ops (and the two snapshots, whose only reader the restore is) are left out.
enum class SpanWhy : uint8_t { Taken, Unusable, Static, Pattern, Overlap, Readers, Written, Stencil, NoColourPair, ColourStale, NoDepthPair, DepthStale, Count };
inline const char* SpanWhyName(int w) {
  static const char* const k[] = {"taken", "unusable frame", "G11's static rules", "no whole snapshot copy of the frame buffer before the clear", "an op another plan owns", "the snapshots have other readers (or are lasting / read back)", "the scene target is written in the window",
                                  "the clear carries stencil", "no equal colour pair", "colour pair written since", "no equal depth pair", "depth pair written since", "?"};
  return w >= 0 && w < int(SpanWhy::Count) ? k[w] : "?";
}
struct Span {
  uint32_t c1 = ~0u, c2 = ~0u, clr = ~0u, drw = ~0u;   // the op indexes: colour snapshot copy, depth snapshot copy, the clear, the restore draw
  uint32_t sc = 0, sd = 0;                             // the scene target's colour / depth (game ids)
  uint32_t fc = 0, fd = 0;                             // the sources of the two snapshot copies (the frame buffer's colour / depth)
  uint32_t x = 0, y = 0;                               // the snapshots' destinations (the restore's colour / depth textures)
  uint32_t w = 0, h = 0;
  uint8_t state = 0;                                   // 0 planned, 1 taken, 2 declined
  bool clear_stencil = false;
};

struct Census {
  uint64_t frames = 0, frames_unusable = 0, frames_taken = 0;
  uint64_t restore_seen = 0, identity_restore = 0, identity_blit = 0;
  uint64_t why[int(Why::Count)] = {};
  uint64_t static_why[int(frame_cuts::Why::Count)] = {};   // under "G11's static rules": the rule that declined
  uint64_t aliased = 0;                                    // step 2: spans taken (the scene target's round trip left out; "aliased" in the brief's census line)
  uint64_t plan_ns = 0, plan_frames = 0;                   // the span plan's own wall time (one steady_clock pair a frame, never per draw)
  uint64_t span_planned = 0, span_why[int(SpanWhy::Count)] = {}, clears_dropped = 0, violations = 0;
  uint64_t copies_dropped = 0, passes_dropped = 0;
  uint64_t px = 0;                                         // pixels the taken draws would have written
};

// review F2: the ledger (C5's dr_stale_d_ / dr_banned_ as a class): the snapshots a taken span stopped writing are stale from that frame on; a later frame whose stream READS one before the stream writes it is a violation:
// the image is banned for good and no span takes it again.
class Ledger {
 public:
  void Taken(uint32_t x, uint32_t y) { stale_.insert(x), stale_.insert(y); }
  void Written(uint32_t id) { stale_.erase(id); }
  bool Read(uint32_t id) {   // true = a violation (counted by the caller)
    if (!id || !stale_.count(id) || g_fault == Fault::NoLedger) return false;
    banned_.insert(id), stale_.erase(id);
    return true;
  }
  bool Banned(uint32_t id) const { return banned_.count(id) != 0; }
  bool Empty() const { return stale_.empty(); }
 private:
  std::unordered_set<uint32_t> stale_, banned_;
};

class RoundTrip {
 public:
  Census census;
  bool usable = false;

  void BeginFrame(bool ok) {
    ++census.frames;
    usable = ok;
    if (!ok) ++census.frames_unusable;
    frame_taken_ = false;
    ver_.clear();
    pairs_.clear();
    dirty_.clear();
    pending_.clear();
    have_targets_ = false;
    color_count_ = 0;
    depth_ = 0;
  }
  void MarkDirty(uint32_t image) { if (image) dirty_.insert(image); }   // a CPU upload into it this frame

  // The walk's calls at the TOP of each op (the previous op's writes are applied now, this op's wait for the next op): the same order as frame_cuts::Tracker.
  void OpSetTargets(const uint32_t* color, uint32_t count, uint32_t depth) {
    Flush();
    color_count_ = std::min<uint32_t>(count, 4);
    for (uint32_t c = 0; c < color_count_; ++c) color_[c] = color[c];
    depth_ = depth;
    have_targets_ = true;
  }
  void OpClear() { Flush(), WriteTargets(true, false); }
  // `writes_depth`: the draw's state can change the depth target (depth test + depth write, or stencil test).
  void OpDraw(bool writes_depth) { Flush(), WriteTargets(writes_depth, true); }
  void OpCopy(uint32_t dest) { Flush(), Write(dest); }
  void Foreign(uint32_t image) { Write(image); }   // another plan writes this image at this point (the post group's pass)
  void Flush() {
    for (uint32_t image : pending_) ++ver_[image];
    pending_.clear();
  }
  // The draw of this op is left out: it writes nothing. (Its writes were queued by OpDraw at the top of the op and not applied yet.)
  void CancelDraw() { pending_.clear(); }

  // After the walk handled a copy: a whole copy the walk really issued makes the two images equal.
  void CopyDone(uint32_t src, uint32_t dst, bool issued, bool whole) {
    if (!whole || !issued || src == dst) return;
    Flush();
    AddPair(src, dst, 0);
  }

  // The restore the walk is about to decide on (G11's static rules passed: `v.kind == Restore`): is it the identity? On false the reason is in `why`.
  bool RestoreIdentity(const frame_cuts::Verdict& v, Why& why) {
    ++census.restore_seen;
    if (!usable) return why = Why::Unusable, Count(why), false;
    if (g_fault == Fault::NoPair) return why = Why::Taken, true;
    const State c = Equal(v.colour_src, v.colour_dst);
    if (c != State::Ok) return why = c == State::No ? Why::NoColourPair : Why::ColourStale, Count(why), false;
    if (g_fault != Fault::WrongDepth) {
      const State d = Equal(v.depth_src, v.depth_dst);
      if (d != State::Ok) return why = d == State::No ? Why::NoDepthPair : Why::DepthStale, Count(why), false;
    }
    why = Why::Taken;
    return true;
  }
  void CountStatic(frame_cuts::Why w) { ++census.restore_seen, ++census.why[int(Why::Static)], ++census.static_why[int(w)]; }
  void CountIdentityRestore(uint64_t pixels, bool g11_restore_on) {
    ++census.identity_restore, ++census.why[0];
    ++census.passes_dropped;
    if (g11_restore_on) census.copies_dropped += 2;   // the two image copies G11's restore switch would have made in the draw's place
    census.px += pixels;
    TakeFrame();
  }
  // A restore that is not the identity ran (as a draw or as G11's two copies): the destinations equal the sources from here (until written). The draw's own pending write is counted.
  void NoteRestore(const frame_cuts::Verdict& v) {
    AddPair(v.colour_src, v.colour_dst, 1);
    AddPair(v.depth_src, v.depth_dst, 1);
  }

  // ---- the span (step 2) --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  std::vector<Span> spans;
  void ClearSpans() { spans.clear(); }
  // At the first snapshot copy (the walk's op, state before it): is the frame buffer exactly the scene target now (a pair made by the first restore, nothing written to either since: the versions say it)?
  // Ids are the mapped ones (the walk's Map()). On Taken the span is booked taken: every op of it is left out from here.
  SpanWhy AcceptSpan(Span& s, uint32_t fc, uint32_t sc, uint32_t fd, uint32_t sd) {
    SpanWhy why = SpanWhy::Taken;
    if (!usable) why = SpanWhy::Unusable;
    else if (g_fault != Fault::NoPair) {
      const State c = Equal(fc, sc);
      const State d = Equal(fd, sd);
      if (c != State::Ok) why = c == State::No ? SpanWhy::NoColourPair : SpanWhy::ColourStale;
      else if (d != State::Ok && g_fault != Fault::WrongDepth) why = d == State::No ? SpanWhy::NoDepthPair : SpanWhy::DepthStale;
    }
    ++census.span_why[int(why)];
    s.state = why == SpanWhy::Taken ? 1 : 2;
    return why;
  }
  // The pair of the span still holds at the second snapshot (review F1's tripwire; the static plan makes it true).
  bool SpanStillEqual(uint32_t fc, uint32_t sc, uint32_t fd, uint32_t sd) {
    Flush();
    return g_fault == Fault::NoPair || (Equal(fc, sc) == State::Ok && Equal(fd, sd) == State::Ok);
  }
  void CountSpanFound() { ++census.span_planned; }   // a pattern (clear + restore of one target) the plan found: it ends in exactly one reason below
  void CountSpanPlanDecline(SpanWhy why) { ++census.span_why[int(why)]; }
  void CountSpanTaken(const Span& s, bool g11_restore_on) {
    ++census.aliased, ++census.clears_dropped;
    // (the two snapshots are booked by the walk as they are dropped: review F1) the restore: a pass under the stream, two image copies under G11's restore switch (review F6: never both)
    if (g11_restore_on) census.copies_dropped += 2;
    else ++census.passes_dropped;
    census.px += uint64_t(s.w) * s.h;
    TakeFrame();
  }

  std::string Report(int level) const {
    char b[1900];
    uint64_t declined = 0;
    for (int w = 1; w < int(Why::Count); ++w) declined += census.why[w];
    int n = std::snprintf(b, sizeof b, "[rdr][frame_roundtrip] level %d, frames %llu (unusable %llu, taken in %llu), identity %llu (restore %llu, blit %llu; %.2f Mpx), aliased %llu, declined %llu (of %llu restores seen; by reason:",
                          level, static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_unusable), static_cast<unsigned long long>(census.frames_taken),
                          static_cast<unsigned long long>(census.identity_restore + census.identity_blit), static_cast<unsigned long long>(census.identity_restore), static_cast<unsigned long long>(census.identity_blit),
                          double(census.px) / 1e6, static_cast<unsigned long long>(census.aliased), static_cast<unsigned long long>(declined), static_cast<unsigned long long>(census.restore_seen));
    for (int w = 1; w < int(Why::Count); ++w) {
      if (!census.why[w] || n >= int(sizeof b) - 100) continue;
      n += std::snprintf(b + n, sizeof b - size_t(n), " %s %llu", WhyName(w), static_cast<unsigned long long>(census.why[w]));
      if (w == int(Why::Static)) {
        n += std::snprintf(b + n, sizeof b - size_t(n), " [");
        for (int s = 1; s < int(frame_cuts::Why::Count); ++s)
          if (census.static_why[s] && n < int(sizeof b) - 80) n += std::snprintf(b + n, sizeof b - size_t(n), "%s %llu; ", frame_cuts::WhyName(s), static_cast<unsigned long long>(census.static_why[s]));
        n += std::snprintf(b + n, sizeof b - size_t(n), "]");
      }
      n += std::snprintf(b + n, sizeof b - size_t(n), ",");
    }
    n += std::snprintf(b + n, sizeof b - size_t(n), "), spans (scene round trip; the `aliased` above) planned %llu, taken %llu (by reason of the rest:", static_cast<unsigned long long>(census.span_planned),
                       static_cast<unsigned long long>(census.span_why[0]));
    for (int w = 1; w < int(SpanWhy::Count); ++w)
      if (census.span_why[w] && n < int(sizeof b) - 100) n += std::snprintf(b + n, sizeof b - size_t(n), " %s %llu,", SpanWhyName(w), static_cast<unsigned long long>(census.span_why[w]));
    n += std::snprintf(b + n, sizeof b - size_t(n), "), clears dropped %llu, violations %llu), plan %.1f us a frame, copies dropped %llu, passes dropped %llu%s", static_cast<unsigned long long>(census.clears_dropped),
                       static_cast<unsigned long long>(census.violations), census.plan_frames ? double(census.plan_ns) / 1e3 / double(census.plan_frames) : 0.0, static_cast<unsigned long long>(census.copies_dropped), static_cast<unsigned long long>(census.passes_dropped), g_fault != Fault::None ? " FAULT" : "");
    return b;
  }

  enum class State : uint8_t { No, Stale, Ok };
  // Do `a` and `b` hold the same bits now? Ok: a pair made when both had the versions they have now.
  State Equal(uint32_t a, uint32_t b) const {
    if (!a || !b || a == b) return State::No;
    if (dirty_.count(a) || dirty_.count(b)) return State::Stale;
    bool seen = false;
    for (const Pair& p : pairs_) {
      if (!((p.a == a && p.b == b) || (p.a == b && p.b == a))) continue;
      seen = true;
      const uint32_t va = p.a == a ? p.va : p.vb, vb = p.a == a ? p.vb : p.va;
      if (g_fault == Fault::IdentityAfterWrite || (Ver(a) == va && Ver(b) == vb)) return State::Ok;
    }
    return seen ? State::Stale : State::No;
  }

 private:
  struct Pair { uint32_t a = 0, b = 0, va = 0, vb = 0; };
  uint32_t Ver(uint32_t image) const {
    const auto v = ver_.find(image);
    return v == ver_.end() ? 0 : v->second;
  }
  uint32_t Pending(uint32_t image) const { return uint32_t(std::count(pending_.begin(), pending_.end(), image)); }
  void AddPair(uint32_t a, uint32_t b, int own_write) {
    if (!a || !b || a == b) return;
    // the destination `b` now holds the source's bits: an older pair naming it is stale anyway (its write moved the version, or will at the next op); forget them (stale pairs elsewhere stay: the decline reason says "written since")
    pairs_.erase(std::remove_if(pairs_.begin(), pairs_.end(), [&](const Pair& p) { return p.a == b || p.b == b; }), pairs_.end());
    if (pairs_.size() >= 32) pairs_.erase(pairs_.begin());
    Pair p;
    p.a = a, p.b = b, p.va = Ver(a) + (own_write ? Pending(a) : 0), p.vb = Ver(b) + (own_write ? Pending(b) : 0);
    // the closure: whatever `a` equals right now, `b` equals too (S == 2 and 2 == 8 make S == 8: a later write to 2 or 8 does not take that away from S and the snapshot of 8)
    std::vector<Pair> more;
    for (const Pair& q : pairs_) {
      if (q.a != a && q.b != a) continue;
      const uint32_t o = q.a == a ? q.b : q.a, vo = q.a == a ? q.vb : q.va, va = q.a == a ? q.va : q.vb;
      if (o == b || Ver(o) != vo || Ver(a) != va) continue;
      Pair c;
      c.a = o, c.b = b, c.va = vo, c.vb = p.vb;
      more.push_back(c);
    }
    pairs_.push_back(p);
    for (const Pair& c : more) pairs_.push_back(c);
  }
  void Write(uint32_t image) { if (image) pending_.push_back(image); }
  // A draw or a clear writes the current targets. With no target switch seen yet the targets are unknown: the frame is unusable from here on.
  void WriteTargets(bool depth, bool is_draw) {
    if (!have_targets_) {
      if (usable) usable = false, ++census.frames_unusable;
      return;
    }
    if (is_draw && g_fault == Fault::HiddenAsProof) return;   // (planted: no draw counts as a writer)
    for (uint32_t c = 0; c < color_count_; ++c) Write(color_[c]);
    if (depth) Write(depth_);
  }
  void TakeFrame() {
    if (!frame_taken_) frame_taken_ = true, ++census.frames_taken;
  }
  void Count(Why w) { ++census.why[int(w)]; }
  std::unordered_map<uint32_t, uint32_t> ver_;
  std::vector<Pair> pairs_;
  std::unordered_set<uint32_t> dirty_;
  std::vector<uint32_t> pending_;
  bool have_targets_ = false, frame_taken_ = false;
  uint32_t color_[4] = {}, color_count_ = 0, depth_ = 0;
};

}  // namespace rdr_adapter::frame_roundtrip
