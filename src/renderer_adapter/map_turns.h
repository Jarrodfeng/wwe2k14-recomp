// S102 lane G3 (docs\renderer\MAP_TURNS.md): the offscreen maps take turns under LOW. No game or SDK includes (fview / rplay / the live sink include it).
//
// Today's LOW half rate (binder.h g_shadow_half) leaves out the whole learned shadow family on every odd frame: the GPU alternates ~4 ms with ~0 ms. Here each map family has
// its own turn, so the cost alternates inside the frame instead of between frames, and no map is more than ONE DRAWN frame old:
//   wrestler  the wrestler shadow maps A / B (T_ShadowMap, pass_site 0x82C04634, no circle blob)   drawn on EVEN drawn frames, left out on odd
//   rim       the rim light map and its chain (pass_site 0x82BFBD58, role RimMap)                  with the wrestler chain (the two biggest primitive blocks land on one phase,
//                                                                                                   props on the other: R7 section 4)
//   props     the props map + circle blobs (call_site 0x828131E4) and the ring plane mask          drawn on ODD drawn frames, left out on even
//   prelight  the five yPreLight slot passes (dead_work.h kPreLightPassSite)                       slot k drawn when (drawn + k) is even: slots alternate
//   SWEAT / the feedback maps (role FeedbackMap) ACCUMULATE (each frame adds to the last): NEVER in a turn. A planted fault puts them in one; the check must fail.
// Every switch is its own PerFrame switch, default off (rdr_switches.h). "Drawn frames" are binds that are a guest frame's draw (not a lasting-only bind, an isolate, or the
// second part of a frame split at a readback), counted by the binder (binder.h drawn_frame_): a sink-skipping machine (the Surface: 60 swaps, 28-53 shown) alternates
// per SHOWN frame, not per bind (R7 hazard H1).
//
// The planner works on one LoadedFrame like dead_work.h: it returns a per-op drop plan (a SetTargets up to the next one = a span; a skipped span's ops and every copy whose
// source is a skipped target are not recorded). Learning is by draw ORIGIN (shadow_roles.h's rule: never a shader hash) and sticky across frames.
// Guards (a pass is left out only when): the warm-up (8 drawn frames) is over; its target was kept-written on the PREVIOUS drawn frame (so the age after the skip is 1);
// no 2D draw also goes into the target (the copy quad: shadow_roles IsChainNot); a frame with draw origins, a plan-able bind.
// LEDGER (like G4's stale_read / copy_alias's Violations): `owed_` = images whose last write we left out (the span's targets and the copy destinations the readers sample).
// A read of an owed image in a LATER drawn frame before a kept write counts stale_read (per group, logged once); the same frame's read is the intended age-1 read.
// The proof asserts stale_read == 0 on every capture and catches the planted faults (a group stuck in its skip phase, the sweat map in a turn).
#pragma once
#include <algorithm>
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
#include "d3_arena.h"   // review F5: the D3 marker call sites (a span holding a marker is never cut)
#include "dead_work.h"
#include "shadow_roles.h"

namespace rdr_adapter::map_turns {

enum Group : uint8_t { kWrestler = 0, kRim, kProps, kPrelight, kSweat, kGroupCount, kNoGroup = 255 };   // kSweat: never a turn (the census / twin only)
inline const char* GroupName(int g) {
  static const char* const k[] = {"wrestler chain", "rim map", "props / circles / plane", "pre-light slots", "sweat (never)"};
  return g >= 0 && g < int(kGroupCount) ? k[g] : "?";
}
// The four turn switches (live: --wwe2k14_rdr_turn_wrestler / _rim / _props / _prelight through rdr_sw, read every frame; fview: --turns MASK). Relaxed atomics: the
// reader's thread writes, the render thread reads.
inline std::atomic<bool> g_on[kPrelight + 1] = {};
inline bool AnyOn() {
  for (const auto& a : g_on)
    if (a.load(std::memory_order_relaxed)) return true;
  return false;
}

// Planted faults (tools only): each must fail the check.
enum class Fault : uint8_t {
  None,
  StuckWrestler,   // the wrestler chain is left out on EVERY drawn frame after the warm-up (a map skipped on the frame its reader needs it)
  StuckRim,
  StuckProps,
  StuckPrelight,
  SweatInTurn,     // the feedback (sweat) maps are put in a turn: left out on odd drawn frames
  NoGuard,         // review F4: the wrestler chain is left out 2 of every 3 drawn frames and the age guard is off: a read sees an image two drawn frames old (the ledger must catch it)
  CutMarkers,      // review F5: the D3-marker guard is off: a span holding a marker draw is cut and its packet is never taken (unconsumed > 0 under --d3 casters)
  BadPhase,        // the same bad phase WITH the age guard: the guard keeps the second skip (stale 0, guard_kept > 0): proves the guard works
  Count
};
inline Fault g_fault = Fault::None;
inline bool ParseFault(const char* name, Fault& out) {
  static const char* const k[] = {"none", "stuck_wrestler", "stuck_rim", "stuck_props", "stuck_prelight", "sweat_in_turn", "no_guard", "cut_markers", "bad_phase"};
  for (int i = 0; i < int(Fault::Count); ++i)
    if (std::strcmp(name, k[i]) == 0) return out = Fault(i), true;
  return false;
}
// Step 1 of the lane: the DRAWN-frame clock. A bind is a new drawn frame unless it is a lasting-only bind (a skipped frame's), an isolate, or the second part of a guest frame split at a
// readback (the first part had more of its frame to come). The half rate (binder.h) and the turns key on this count, never on the number of binds (R7 hazard H1: the Surface skips sink
// frames and every skipped frame that carries lasting work is a bind). tools\g3\g3_clock_test.cpp proves it and a twin that counts binds.
struct DrawnClock {
  uint64_t drawn = 0;
  bool prev_part_open = false;
  uint64_t last_guest = ~0ull;
  // Returns the drawn-frame count this bind belongs to. Review F1: the second bind of a split frame is told by its GUEST FRAME, not by "the last drawn bind was a part": when the rest of
  // a split frame is skipped (a lasting-only bind only), the next DRAWN bind is a new guest frame and must count, or the count freezes for one frame.
  uint64_t Bind(bool persist_only, bool isolate, bool frame_part_open, uint64_t guest_frame) {
    const bool drawn_bind = !persist_only && !isolate;
    if (drawn_bind && (!prev_part_open || guest_frame != last_guest)) ++drawn;
    if (drawn_bind) prev_part_open = frame_part_open, last_guest = guest_frame;
    return drawn;
  }
};
// Step 1 behind its own switch (wwe2k14_rdr_half_rate_drawn_clock, PerFrame, default OFF): the LOW half rate counts drawn frames. Off = the old per-bind count, exactly as before.
inline std::atomic<bool> g_half_rate_drawn_clock{false};
inline uint64_t g_half_clock_offset = 0;   // fview --half-clock-offset N (TEST ONLY): the half rate's count is moved N frames (the step-1-off arm must then differ from the pre-G3 exe)
inline uint32_t g_plant_part_every = 0;   // fview --part-every N (TEST ONLY): every N-th frame is bound as an open part
inline int g_list_frame = -1;   // fview --turns-list N: print the planner's span table of drawn frame N (census, 0 cost when off)
inline constexpr uint64_t kWarmDrawn = 8;
inline constexpr uint32_t kPrelightRun = 4;   // a pre-light slot takes turns only after it was emitted this many drawn frames in a row

struct Census {
  uint64_t frames = 0, frames_planned = 0, frames_unplanned = 0, frames_blind = 0;
  uint64_t spans_seen[kGroupCount] = {}, spans_cut[kGroupCount] = {}, guard_kept[kGroupCount] = {};   // guard_kept: due to be left out, kept (not written last frame / never)
  uint64_t draws_cut[kGroupCount] = {}, copies_cut[kGroupCount] = {}, clears_cut[kGroupCount] = {}, targets_cut[kGroupCount] = {};
  uint64_t reads_age0[kGroupCount] = {}, reads_age1[kGroupCount] = {}, stale_read[kGroupCount] = {};
  uint64_t sweat_skipped = 0;   // must be 0 (only the planted fault makes it > 0)
  uint64_t marker_kept = 0, present_kept = 0;   // spans never cut: a D3 marker draw in them (F5), a Present in them (F10)
  uint64_t StaleTotal() const {
    uint64_t n = 0;
    for (uint64_t v : stale_read) n += v;
    return n;
  }
  uint64_t CutTotal() const {
    uint64_t n = 0;
    for (uint64_t v : spans_cut) n += v;
    return n;
  }
};

struct Plan {
  std::vector<uint8_t> drop;   // by op index: 1 = not recorded
  uint32_t count = 0;
  void Reset(uint32_t ops) { drop.assign(ops, 0), count = 0; }
};

class Planner {
 public:
  Census census;
  uint32_t last_stale_image = 0;
  uint8_t last_stale_group = kNoGroup;

  // `role_of`: names.h SurfaceRole of an image id (0 unknown). `external_drop`: another planner's drop plan for the same frame (null none): its ops are neither reads nor writes here.
  // `plan_ok` false: a bind that must not be planned (a lasting-only bind, a split part, the one after a split part); `ledger_ok` false: no guest-frame draw at all (isolate / velocity).
  // `drawn`: the binder's drawn-frame counter for this bind (counts DRAWN frames, step 1 of the lane).
  void Build(const LoadedFrame& f, uint64_t drawn, const std::function<uint8_t(uint32_t)>& role_of, const std::vector<uint8_t>* external_drop, bool plan_ok, bool ledger_ok, Plan& plan, const std::function<bool(uint32_t)>& old_skip = nullptr) {
    plan.Reset(f.ops.count);
    owned_.clear();
    if (!ledger_ok || !f.ops.count) return;
    ++census.frames;
    if (!AnyOn() && g_fault == Fault::None) return;
    if (!f.origins.count) {
      ++census.frames_blind;
      return;
    }
    Spans(f, role_of);
    Learn(f, role_of);
    for (const Span& s : spans_)   // review F2: on EVERY bind (a split part is unplanned): the old half rate leaves the groups whose switch is on to the turns
      if ((s.group == kWrestler || s.group == kRim || s.group == kProps) && !s.follower && On(s.group) && !never_.count(s.key)) owned_.insert(s.key);
    if (plan_ok) {
      ++census.frames_planned;
      Decide(f, drawn, role_of, plan);
    } else {
      ++census.frames_unplanned;
    }
    Ledger(f, drawn, plan, external_drop, old_skip);
    if (g_list_frame >= 0 && uint64_t(g_list_frame) == drawn) List(f, drawn, plan);
  }

  // Is this image a target the new turns own this frame (its switch is on)? The old half rate leaves it to the turns.
  bool Owns(uint32_t image) const { return owned_.count(image) != 0; }
  // A target of one of the turn groups (learned, sticky): copy_alias leaves copies out of it alone (review F7).
  bool Tracks(uint32_t image) const { return group_of_.count(image) != 0; }

  std::string Report() const {
    char b[700];
    std::string out;
    const unsigned long long frames = census.frames_planned ? census.frames_planned : 1;
    std::snprintf(b, sizeof b, "[rdr][turns] switches wrestler %d rim %d props %d prelight %d%s: frames %llu planned %llu unplanned %llu blind %llu, STALE READS %llu, sweat skipped %llu, spans kept (D3 marker %llu, Present %llu)\n",
                  g_on[kWrestler].load(), g_on[kRim].load(), g_on[kProps].load(), g_on[kPrelight].load(), g_fault == Fault::None ? "" : " FAULT",
                  static_cast<unsigned long long>(census.frames), static_cast<unsigned long long>(census.frames_planned), static_cast<unsigned long long>(census.frames_unplanned),
                  static_cast<unsigned long long>(census.frames_blind), static_cast<unsigned long long>(census.StaleTotal()), static_cast<unsigned long long>(census.sweat_skipped), static_cast<unsigned long long>(census.marker_kept), static_cast<unsigned long long>(census.present_kept));
    out += b;
    for (int g = 0; g < int(kGroupCount); ++g) {
      std::snprintf(b, sizeof b, "[rdr][turns]   %-24s spans seen %llu, left out %llu (%.2f passes, %.1f draws, %.1f copies a planned frame; kept by a guard %llu), reads age 0 %llu age 1 %llu STALE %llu%s", GroupName(g),
                    static_cast<unsigned long long>(census.spans_seen[g]), static_cast<unsigned long long>(census.spans_cut[g]), double(census.spans_cut[g]) / double(frames),
                    double(census.draws_cut[g]) / double(frames), double(census.copies_cut[g]) / double(frames), static_cast<unsigned long long>(census.guard_kept[g]),
                    static_cast<unsigned long long>(census.reads_age0[g]), static_cast<unsigned long long>(census.reads_age1[g]), static_cast<unsigned long long>(census.stale_read[g]),
                    g + 1 < int(kGroupCount) ? "\n" : "");
      out += b;
    }
    return out;
  }

 private:
  struct Span {
    uint32_t begin = 0, end = 0;
    uint32_t key = 0;                       // the colour target, else the depth target (what the old half rate keyed on)
    uint8_t role = 0;
    bool map = false, circ = false, plane = false, rim = false, two_d = false, prelight = false;
    bool marker = false, present = false, family_only = true;   // marker: a D3 marker draw (F5); present: a Present op inside (F10); family_only: every named draw's origin is in the shadow family (F8)
    uint32_t draws = 0, copies = 0, first_dest = 0;
    std::vector<uint32_t> reads;            // the images its draws sample (sorted, unique)
    std::vector<uint32_t> dests;            // its copy destinations
    bool follower = false;                  // not a map itself: a blur / reduction step that samples ONLY the group's maps and earlier followers (T_ShadowMapBlur, the plane's downscale, the rim's blur)
    int slot = -1;                          // prelight: a stable slot index by its first copy destination
    uint8_t group = kNoGroup;               // decided by Learn
    bool cut = false;
  };
  std::vector<Span> spans_;
  std::unordered_map<uint32_t, uint8_t> group_of_;        // key image -> group (sticky)
  std::unordered_set<uint32_t> never_;                    // key images a 2D draw / a conflict also uses: never left out
  std::unordered_map<uint32_t, uint32_t> slot_of_;        // prelight: first copy destination -> slot
  std::unordered_map<uint32_t, uint8_t> dest_group_;      // a copy destination of a group's target (what the readers sample) -> group
  std::unordered_map<uint32_t, uint64_t> written_;        // image -> the drawn frame of its last kept write
  std::unordered_map<uint32_t, uint64_t> owed_;           // image -> the drawn frame whose write was left out (the ledger)
  std::unordered_set<uint32_t> owned_;                    // this frame: key images of the groups whose switch is on (old half rate stays off them)
  std::unordered_set<uint32_t> skipped_keys_;
  bool rim_seen_ = false;
 public:
  std::function<uint64_t(uint32_t)> px_of;   // image id -> pixel count (0 unknown): the binder sets it (review F9)
 private:
  uint32_t slot_run_[16] = {};   // pre-light: consecutive planned frames the slot's pass was emitted by the game (the game re-bakes a slot only when its light rows change)

  // The shadow family's origins (review F8): the map / plane / rim passes, the circle blob, and the shadow blur (T_ShadowMapBlur, call 0x82814080: the min_rgba into map B).
  static bool InFamily(const DrawOrigin* o) {
    return o->pass_site == shadow_roles::kMapPassSite || o->pass_site == shadow_roles::kPlanePassSite || o->pass_site == shadow_roles::kRimPassSite || o->call_site == shadow_roles::kCircleCallSite || o->call_site == kBlurCallSite;
  }
  static constexpr uint32_t kBlurCallSite = 0x82814080u;   // [C] docs\renderer\SHADOW_PROMOTION_MAP.md section 1.1 (T_ShadowMapBlur call)

  static bool On(uint8_t g) { return g <= kPrelight && g_on[g].load(std::memory_order_relaxed); }

  void Spans(const LoadedFrame& f, const std::function<uint8_t(uint32_t)>& role_of) {
    spans_.clear();
    const uint32_t n = f.ops.count;
    for (uint32_t p = 0; p < n;) {
      if (f.ops[p].kind != OpKind::SetTargets) { ++p; continue; }
      Span s;
      s.begin = p;
      uint32_t q = p + 1;
      while (q < n && f.ops[q].kind != OpKind::SetTargets) ++q;
      s.end = q;
      const rdr::TargetsOp& t = f.targets[f.ops[p].index];
      s.key = t.color_count ? t.color[0].image : t.depth.image;
      s.role = s.key ? role_of(s.key) : 0;
      for (uint32_t i = p + 1; i < q; ++i) {
        const Op& op = f.ops[i];
        if (op.kind == OpKind::Draw) {
          ++s.draws;
          { const Draw& dd = f.draws[op.index]; for (uint32_t k = 0; k < dd.texture_count; ++k) s.reads.push_back(f.texture_slots[dd.texture_first + k].image); }
          const DrawOrigin* o = f.OriginOf(op.index);
          if (!shadow_roles::Named(o)) continue;
          if (o->pass_site == shadow_roles::kMapPassSite) s.map = true;
          if (o->call_site == shadow_roles::kCircleCallSite) s.circ = true;
          if (o->pass_site == shadow_roles::kPlanePassSite) s.plane = true;
          if (o->pass_site == shadow_roles::kRimPassSite) s.rim = true;
          if (shadow_roles::Is2D(o->klass)) s.two_d = true;
          if (o->pass_site == dead_work::kPreLightPassSite || o->call_site == dead_work::kPreLightCallSite) s.prelight = true;
          if (d3::IsMarkerCall(o->call_site) || d3::IsRingMarkerCall(o->call_site) || d3::IsWrestlerMarkerCall(o->call_site) || d3::IsCasterMarkerCall(o->call_site)) s.marker = true;
          if (!InFamily(o)) s.family_only = false;
        } else if (op.kind == OpKind::Present) {
          s.present = true;
        } else if (op.kind == OpKind::Copy) {
          if (!s.copies++) s.first_dest = f.copies[op.index].dest.image;
          s.dests.push_back(f.copies[op.index].dest.image);
        }
      }
      std::sort(s.reads.begin(), s.reads.end());
      s.reads.erase(std::unique(s.reads.begin(), s.reads.end()), s.reads.end());
      spans_.push_back(std::move(s));
      p = q;
    }
  }

  void Learn(const LoadedFrame& f, const std::function<uint8_t(uint32_t)>& role_of) {
    (void)f;
    for (Span& s : spans_) {
      if (!s.key) continue;
      if (s.rim) rim_seen_ = true;
      if (s.role == uint8_t(SurfaceRole::FeedbackMap)) { s.group = kSweat; ++census.spans_seen[kSweat]; continue; }
      if (s.prelight && s.first_dest) {
        auto it = slot_of_.find(s.first_dest);
        if (it == slot_of_.end() && slot_of_.size() < 16) it = slot_of_.emplace(s.first_dest, uint32_t(slot_of_.size())).first;
        if (it != slot_of_.end()) s.slot = int(it->second);
        s.group = kPrelight;
        ++census.spans_seen[kPrelight];
        continue;
      }
      uint8_t learned = kNoGroup;
      if (s.circ || s.plane) learned = kProps;
      else if (s.map) learned = kWrestler;
      const bool rim_span = s.rim || (rim_seen_ && s.role == uint8_t(SurfaceRole::RimMap));
      if (rim_span) {
        if (learned != kNoGroup) never_.insert(s.key);   // a rim chain target a caster also draws into: never left out
        else learned = kRim;
      }
      auto it = group_of_.find(s.key);
      if (learned != kNoGroup) {
        if (it == group_of_.end()) it = group_of_.emplace(s.key, learned).first;
        else if (it->second == learned) {}
        else if (it->second == kWrestler && learned == kProps) it->second = kProps;   // the props map first looked like a wrestler map: its circle blobs say props
        else if (it->second == kProps && learned == kWrestler) {}                      // a props-map span with no circle in it stays props
        else never_.insert(s.key);                                                     // two families in one target: never
      }
      if (s.two_d && learned != kRim && it != group_of_.end() && it->second != kRim) never_.insert(s.key);   // the copy quad also draws here (shadow_roles IsChainNot)
      if (it != group_of_.end()) {
        if (learned == kNoGroup && !s.family_only) {   // review F8: a span into a group's target whose draws are not the shadow family's (a scene / UI draw into the same image): the key is never left out
          never_.insert(s.key);
          continue;
        }
        s.group = it->second;
        ++census.spans_seen[s.group];
      }
    }
    Followers(role_of);
  }

  // A blur / reduction step of a map chain: a span with no group whose draws sample only images the chain wrote (its maps' targets and copy destinations, and earlier followers'),
  // whose own target is written by no span outside the chain, and whose target role is not a scene / depth / present one. It follows its group's turn (left out with it) so the
  // chain's small passes do not redo last frame's picture from last frame's maps. Frame-local (the chain's structure is read from this frame's ops).
  void Followers(const std::function<uint8_t(uint32_t)>& role_of) {
    std::unordered_set<uint32_t> chain[kPrelight];
    uint64_t chain_px[kPrelight] = {};   // the largest map of the chain, in pixels (review F9)
    std::unordered_map<uint32_t, int> outside;   // key -> spans that are neither group spans nor followers (counted after detection)
    for (Span& s : spans_) {
      if (s.group == kWrestler || s.group == kRim || s.group == kProps) {
        if (never_.count(s.key)) continue;
        chain[s.group].insert(s.key);
        for (uint32_t d : s.dests) chain[s.group].insert(d);
        if (px_of) chain_px[s.group] = std::max<uint64_t>(chain_px[s.group], px_of(s.key));
        continue;
      }
      if (s.group != kNoGroup || !s.key || !s.draws || s.two_d || s.reads.empty()) continue;
      const uint8_t r = role_of(s.key);
      const bool role_ok = r == uint8_t(SurfaceRole::ShadowMap) || r == uint8_t(SurfaceRole::RimMap);   // review F9: a named shadow / rim role, or (an unnamed target) one no larger than the chain's maps
      if (r != 0 && !role_ok) continue;
      for (int g = 0; g < int(kPrelight); ++g) {
        if (chain[g].empty()) continue;
        if (!role_ok && !(px_of && px_of(s.key) && px_of(s.key) <= chain_px[g])) continue;
        bool all = true;
        for (uint32_t i : s.reads) all = all && chain[g].count(i);
        if (!all) continue;
        s.group = uint8_t(g);
        s.follower = true;
        chain[g].insert(s.key);
        for (uint32_t d : s.dests) chain[g].insert(d);
        break;
      }
    }
    for (const Span& s : spans_)
      if (!s.follower && s.key && !(s.group == kWrestler || s.group == kRim || s.group == kProps)) ++outside[s.key];
    for (Span& s : spans_)   // a follower whose target another (non-chain) span also draws into is not one
      if (s.follower && outside.count(s.key)) { s.follower = false; s.group = kNoGroup; }
    for (const Span& s : spans_)
      if (s.follower) ++census.spans_seen[s.group];
  }

  static bool Stuck(uint8_t g) {
    const Fault f = g_fault;
    return (g == kWrestler && f == Fault::StuckWrestler) || (g == kRim && f == Fault::StuckRim) || (g == kProps && f == Fault::StuckProps) || (g == kPrelight && f == Fault::StuckPrelight);
  }

 public:
  static bool Phase(uint8_t g, int slot, uint64_t drawn) {   // true = left out on this drawn frame (the turn table; public for the g3 clock test)
    if (Stuck(g)) return true;
    if (g == kWrestler && (g_fault == Fault::NoGuard || g_fault == Fault::BadPhase)) return drawn % 3 != 0;   // planted: two of every three drawn frames
    switch (g) {
      case kWrestler:
      case kRim: return (drawn & 1) != 0;
      case kProps: return (drawn & 1) == 0;
      case kPrelight: return ((drawn + uint64_t(slot < 0 ? 0 : slot)) & 1) != 0;
      default: return false;
    }
  }
 private:

  bool WrittenLast(uint32_t image, uint64_t drawn) const {
    const auto it = written_.find(image);
    return it != written_.end() && it->second + 1 == drawn;
  }

  void Decide(const LoadedFrame& f, uint64_t drawn, const std::function<uint8_t(uint32_t)>& role_of, Plan& plan) {
    (void)role_of;
    skipped_keys_.clear();
    uint32_t prev_run[16];   // the pre-light slots' emission runs BEFORE this frame (the decision below), then updated
    bool slot_seen[16] = {};
    for (const Span& s : spans_)
      if (s.group == kPrelight && s.slot >= 0 && s.slot < 16) slot_seen[s.slot] = true;
    for (int k = 0; k < 16; ++k) {
      prev_run[k] = slot_run_[k];
      slot_run_[k] = slot_seen[k] ? slot_run_[k] + 1 : 0;
    }
    if (drawn <= kWarmDrawn) return;
    bool group_cut[kGroupCount] = {};
    for (int pass = 0; pass < 2; ++pass)   // 0: the maps themselves, 1: their followers (only where the group's map was left out this frame)
    for (Span& s : spans_) {
      if (s.group == kNoGroup || !s.key || s.follower != (pass == 1)) continue;
      if (s.follower && !group_cut[s.group]) continue;
      if (s.marker && g_fault != Fault::CutMarkers) { ++census.marker_kept; continue; }    // review F5: a D3 marker draw stands for an object the game did not build; its packet is taken by the bind that records it
      if (s.present) { ++census.present_kept; continue; }   // review F10: never drop a Present
      bool want;
      if (s.group == kSweat) {
        if (g_fault != Fault::SweatInTurn) continue;
        want = (drawn & 1) != 0;
      } else {
        const bool stuck = Stuck(s.group) || (g_fault == Fault::CutMarkers && s.group == kWrestler);   // planted: left out on every drawn frame, no guard
        want = (On(s.group) || stuck) && Phase(s.group, s.slot, drawn);
        if (want && !stuck && s.group != kPrelight && never_.count(s.key)) want = false;
        if (want && !stuck && g_fault != Fault::NoGuard) {   // the age guard: written on the previous drawn frame (so the age after leaving it out is 1)
          const uint32_t probe = s.group == kPrelight ? s.first_dest : s.key;
          if (!WrittenLast(probe, drawn)) { ++census.guard_kept[s.group]; want = false; }
          // pre-light: the game bakes a slot only when its light rows change (a match: 6 frames of 502). A bake left out when the game does not re-emit it the next frame stays
          // wrong until the lights change again (the ledger found exactly this on the match capture). So only a slot that has been emitted on each of the last kPrelightRun drawn frames
          // (an animating entrance rig) takes turns.
          else if (s.group == kPrelight && !(s.slot >= 0 && s.slot < 16 && prev_run[s.slot] >= kPrelightRun)) { ++census.guard_kept[s.group]; want = false; }
        }
      }
      if (!want) continue;
      s.cut = true;
      group_cut[s.group] = true;
      ++census.spans_cut[s.group];
      if (s.group == kSweat) ++census.sweat_skipped;
      if (s.group != kPrelight) skipped_keys_.insert(s.key);
      for (uint32_t p = s.begin; p < s.end; ++p) {
        plan.drop[p] = 1;
        ++plan.count;
        switch (f.ops[p].kind) {
          case OpKind::Draw: ++census.draws_cut[s.group]; break;
          case OpKind::Copy: ++census.copies_cut[s.group]; break;
          case OpKind::Clear: ++census.clears_cut[s.group]; break;
          case OpKind::SetTargets: ++census.targets_cut[s.group]; break;
          default: break;
        }
      }
    }
    if (skipped_keys_.empty()) return;
    for (uint32_t p = 0; p < f.ops.count; ++p) {   // a copy out of a left-out target (outside its span) is left out too
      if (plan.drop[p] || f.ops[p].kind != OpKind::Copy) continue;
      if (skipped_keys_.count(f.copies[f.ops[p].index].source.image)) {
        plan.drop[p] = 1;
        ++plan.count;
        const auto it = group_of_.find(f.copies[f.ops[p].index].source.image);
        ++census.copies_cut[it == group_of_.end() ? kWrestler : it->second];
      }
    }
  }

  // The ledger: kept writes clear `owed_`, left-out writes set it, a read of an owed image after the frame that left it out is a stale read.
  // `old_skip(image)`: the binder's old all-at-once half rate (OldHalfSkips) for this frame: the ops it leaves out are dropped writes here too (review F2: the ledger saw only our own plan).
  void Ledger(const LoadedFrame& f, uint64_t drawn, const Plan& plan, const std::vector<uint8_t>* external, const std::function<bool(uint32_t)>& old_skip) {
    const auto group_of_image = [&](uint32_t image) -> uint8_t {
      auto it = group_of_.find(image);
      if (it != group_of_.end()) return it->second;
      auto d = dest_group_.find(image);
      return d != dest_group_.end() ? d->second : uint8_t(kNoGroup);
    };
    const auto wrote = [&](uint32_t image) {
      if (!image) return;
      written_[image] = drawn;
      owed_.erase(image);
    };
    const auto owe = [&](uint32_t image, uint8_t g) {
      if (!image) return;
      owed_.emplace(image, drawn);   // the FIRST frame whose write was left out (a second skip must not make it look fresh)
      if (g != kNoGroup) dest_group_[image] = g;
    };
    const auto read = [&](uint32_t image) {
      if (!image) return;
      const auto it = owed_.find(image);
      const uint8_t g = group_of_image(image);
      if (it != owed_.end()) {
        const uint8_t gg = g == kNoGroup ? kWrestler : g;
        // review F4: the age of what the reader sees = drawn frames since the last KEPT write; stale when it is above 1 or the image was never written
        const auto w = written_.find(image);
        const bool never = w == written_.end();
        const uint64_t age = never ? ~0ull : drawn - w->second;
        if (!never && age <= 1) { ++census.reads_age1[gg]; return; }
        ++census.stale_read[gg];   // the write was left out in an earlier drawn frame and nothing kept wrote it since
        last_stale_image = image;
        last_stale_group = gg;
        owed_.erase(it);
        return;
      }
      if (g != kNoGroup) {
        const auto w = written_.find(image);
        if (w != written_.end() && w->second == drawn) ++census.reads_age0[g];
      }
    };
    uint32_t span = 0;
    const auto group_of_span = [&](uint32_t p) -> uint8_t {
      while (span < spans_.size() && spans_[span].end <= p) ++span;
      return span < spans_.size() && p >= spans_[span].begin ? spans_[span].group : uint8_t(kNoGroup);
    };
    bool old_skipping = false;
    for (uint32_t p = 0; p < f.ops.count; ++p) {
      if (external && p < external->size() && (*external)[p]) continue;   // another planner's: not recorded
      const Op& op = f.ops[p];
      bool dropped = p < plan.drop.size() && plan.drop[p];
      if (old_skip && !dropped) {   // the binder's old half rate: a SetTargets onto a skipped image starts a skipping run to the next SetTargets; a copy out of one is left out
        if (op.kind == OpKind::SetTargets) {
          const rdr::TargetsOp& t = f.targets[op.index];
          old_skipping = old_skip(t.color_count ? t.color[0].image : t.depth.image);
        }
        if (op.kind == OpKind::Copy) dropped = old_skip(f.copies[op.index].source.image);
        else if (op.kind != OpKind::Present && old_skipping) dropped = true;
      }
      const uint8_t sg = group_of_span(p);
      switch (op.kind) {
        case OpKind::SetTargets: {
          const rdr::TargetsOp& t = f.targets[op.index];
          if (dropped) {
            for (uint32_t c = 0; c < t.color_count; ++c) owe(t.color[c].image, sg);
            owe(t.depth.image, sg);
          } else {
            for (uint32_t c = 0; c < t.color_count; ++c) wrote(t.color[c].image);
            wrote(t.depth.image);
          }
          break;
        }
        case OpKind::Draw: {
          if (dropped) break;
          const Draw& d = f.draws[op.index];
          for (uint32_t k = 0; k < d.texture_count; ++k) read(f.texture_slots[d.texture_first + k].image);
          break;
        }
        case OpKind::Copy: {
          const rdr::CopyOp& c = f.copies[op.index];
          if (dropped) {
            owe(c.dest.image, group_of_image(c.source.image) != kNoGroup ? group_of_image(c.source.image) : sg);
          } else {
            read(c.source.image);
            if (group_of_image(c.source.image) != kNoGroup && c.dest.image) dest_group_[c.dest.image] = group_of_image(c.source.image);
            wrote(c.dest.image);
          }
          break;
        }
        case OpKind::Present:
          read(f.presents[op.index].source.image);
          break;
        default: break;
      }
    }
  }

  void List(const LoadedFrame& f, uint64_t drawn, const Plan& plan) const {
    std::printf("[turns][list] drawn frame %llu: %zu spans (age guard: written last frame; never %zu)\n", static_cast<unsigned long long>(drawn), spans_.size(), never_.size());
    for (const Span& s : spans_) {
      std::string rd;
      for (size_t i = 0; i < s.reads.size() && i < 6; ++i) rd += (i ? "," : "") + std::to_string(s.reads[i]);
      std::printf("[turns][list]   reads [%s]\n", rd.c_str());
      std::printf("[turns][list]   ops %u..%u key %u role %u draws %u copies %u dest %u  map %d circ %d plane %d rim %d 2d %d prelight %d slot %d -> %s%s%s%s%s\n", s.begin, s.end, s.key, unsigned(s.role), s.draws, s.copies, s.first_dest,
                  s.map, s.circ, s.plane, s.rim, s.two_d, s.prelight, s.slot, s.group == kNoGroup ? "-" : GroupName(s.group), s.follower ? " (follower)" : "", never_.count(s.key) ? " NEVER" : "", s.cut ? " LEFT OUT" : "",
                  s.group != kNoGroup && s.group <= kPrelight && On(s.group) ? "" : " (switch off)");
    }
    (void)f;
    (void)plan;
  }
};

}  // namespace rdr_adapter::map_turns
