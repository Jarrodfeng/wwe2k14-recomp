// S105 SKB: which queued asset the live loader thread loads next (pure, no locks: the caller holds the queue's mutex).
// The old rule was strict FIFO: the wrestler models that gate READY (a counted miss) waited behind the non-gating props (S105 SKB note: 2.5-4.0 s of black at an entrance start).
// Rule: the OLDEST gating job first (an arena job, or a cache miss with gates = true); when none is queued, the oldest that is not `bulk` (S105 SKB fix F3: the evtobj.pac prewarm, a ~1.7 s unpack the one loader thread cannot be
// pulled out of); when only bulk jobs are queued, the oldest. FIFO among equals.
// No starvation: a gating job is queued once per key (the cache's pending_ set) and the render thread stops asking once the key is resident, so the gating set is finite; a non-gating job waits only while gating jobs exist,
// and a bulk job only while any other job exists (the prewarm is queued once per arena load, so the other jobs are a finite set too).
#pragma once
#include <cstddef>

namespace rdr_scene {

// S105 SKB fix F3: the entry name of the evtobj.pac prewarm job (pac "pac\\evt\\evtobj.pac"): the loader opens the pac so the first entrance prop's ask is a cache hit (live_assets.cpp); the render thread never stores it (AdoptLoaded).
// S105 SKB3: the disc pac is read by blocks now (assets::load_detail::OpenEvtRanged): the prewarm only builds the block index + directory (~25 ms, no longer the ~1.1-1.7 s whole unpack), so the comments below about a ~1.7 s job describe the whole-file FALLBACK.
inline constexpr const char* kEvtobjPrewarmEntry = "prewarm";

// S105 SKB fix F3: which prop asks hold READY (a counted miss: the frame is not shown until the model is resident), by the game's own rule: it shows an entering wrestler WITH his event props, and the ring with its match structure.
// Every kSlotFlagEvent row is a CWeaponEventSD::MakeWeapon model (belt / mic / cap / entrance stage props) and every kSlotFlagStage row a structure of the holder (cage / cell / chamber / ladder rig): both gate.
// Ambient props do not: the m2 match weapons (a weapon pops in), the gm set objects and the blood splats stay non-gating. rf_props.cpp passes this as StoredNamed's count_miss.
enum class PropAsk { M2Weapon, EventProp, StageStructure, GmSet, Blood };
// ONLY in the scene's opening window (`window_open`: no OURS frame of this scene has been drawn yet, or this frame already has another loading miss, i.e. the entrance start / match load): once the scene has drawn, a mid-match ask (a ladder, a table,
// a cage lowering) is non-gating and the prop pops in, so it never holds frames (main session decision, 2026-11-20 evening).
inline constexpr bool PropAskGatesReady(PropAsk k, bool window_open = true) { return window_open && (k == PropAsk::EventProp || k == PropAsk::StageStructure); }

// S105 SKB fix F3: the phone / memory point. The evtobj.pac prewarm (160 MB) is released once the match phase is on (the scene phase word 1 = a match; 4 = an entrance [L: front_end.h phase_word]) and no event prop is wanted or held:
// the game builds event props only in entrances (CWeaponEventSD::MakeWeapon, sub_8272D710, called by the evtobj prop task sub_83391E00); a later one reopens the pac as before this lane. One release per phase-1 stretch
// (`latch`), re-armed when the phase leaves 1. Sets `pending` (weapons::RetryRelease then frees it, retried while the loader lock is busy).
inline void BellRelease(int64_t phase_word, bool want_ev_empty, bool asked_ev_empty, bool& latch, bool& pending) {
  if (phase_word != 1) { latch = false; return; }
  if (want_ev_empty && asked_ev_empty && !latch) latch = pending = true;
}

// Q = a sequence (deque / vector) of jobs with `.arena` (int; >= 0 = a whole-arena load), `.m.gates` (bool) and `.m.bulk` (bool). Returns an index into q, or kNoJob when nothing may start now (q empty, or only bulk jobs while !allow_bulk).
// S105 SKB2: `allow_bulk` = the scene's roster is in (its first OURS frame has drawn: every wrestler model the entrance needs was asked and adopted before that frame). While false a bulk job is NOT started: the prewarm is ~1.7 s on the one
// loader thread and cannot be pre-empted, so a wrestler asked moments after it started waited behind it (runs 128 / 132: 1.4-1.6 s). The prewarm still starts early enough: the entrance intro is seconds long and the first event prop asks later.
inline constexpr size_t kNoJob = size_t(-1);
template <class Q>
inline size_t PickJob(const Q& q, bool allow_bulk = true) {
  size_t i = 0, first_plain = kNoJob, first_bulk = kNoJob;
  for (const auto& j : q) {
    if (j.arena >= 0 || j.m.gates) return i;
    if (j.m.bulk) { if (first_bulk == kNoJob) first_bulk = i; }
    else if (first_plain == kNoJob) first_plain = i;
    ++i;
  }
  if (first_plain != kNoJob) return first_plain;
  return allow_bulk ? first_bulk : kNoJob;
}

}  // namespace rdr_scene
