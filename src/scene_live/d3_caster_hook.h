// D3 skip the pass: the guest-side hook of the caster passes (Session 101, lane 228; docs\renderer\SKIP_GAME_WORK_MAP.md 17.12). Included by draw_origin.h, which owns the three REX_HOOK_RAWs (sub_82C01FE0 map,
// sub_82BF22C8 plane, sub_82BF9878 rim; the tracker's pass-entry record of lane 202 runs FIRST, then this scope decides). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// It mirrors d3_arena_hook.h (the arena's CallScope) with the caster class's own state: the ledger g_cobjs, the packet table / mark set / gate / tally g_cpackets / g_cmarks / g_cgate / g_ctally (d3_arena.h), the class
// switch g_caster_d3 (CallInputs::caster) and the hook switch g_caster_hook_on. A caster call is keyed by the composite (model, pass) (d3::CasterObjectId): one model draws up to three passes a frame, each its own call sequence.
//
// THE SKIP POINT. The pass methods have no direct callers (vtable slots only), so the hook sits ON the method (like sub_82C04DF0's): a skipped call returns to the caller with r3 = 1 ("drew"), exactly what the method's own tail
// returns after its body [C listings 0x82C046FC / 0x82BFBE20 / 0x82BF4DE8]. Skipped paths (PredictPathCaster, d3_guest_core.h): path 1 only ([model+888] == 0 or the pass's bit of [model+600] clear; the MAP's second route to the
// same result: no list in the slot and the collapse flag set). Path 2 (a list in the slot: local = 1 reaches sub_82BFBE30's cache branch) is NOT skipped, paths 3 / 4 are the game's own cache. What a skip does not replay [U, the
// same as the arena's]: sub_82BD5460 / sub_82BD54D0 (a stack local's constructor / destructor around the body), sub_82BCC1C0 / sub_82BCC4D0 (the body's state push / pop, balanced inside the body), sub_82BFBE30's pass
// constants (re-applied by every call). The live verify run reads it (no new unnamed draws, shadows still right).
//
// Default OFF: d3::g_caster_hook_on is false unless --wwe2k14_rdr_caster_d3 turned the chain on (live.cpp) and the record route is built; the gate (3 resets in 10 s) latches it off for the session (binder.h D3CEndFrame).
// Off, the cost is one relaxed load per pass entry. In the VERIFY arm (--wwe2k14_rdr_caster_d3_verify) the marker and packet are written and the original ALWAYS runs: nothing is dropped, the binder compares.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // wwe2k14::g_guest_frame
#include "../guest_read.h"   // GuestWordReadableFast
#include "../renderer_adapter/d3_guest_core.h"
#include "caster_core.h"     // kCollapseTable (the static_assert below)
#include "d3_arena_hook.h"   // Ld32, t_open (the arena's open call: a pass entered inside one is never decided)

REX_EXTERN(sub_82BB10C0);   // the HOOKED op-15 recorder (scene_crowd.h): it opens the origin tag from the thread's t_part and the return address

namespace wwe2k14::d3hook {

static_assert(rdr_adapter::d3::kCasterCollapseTable == rdr_scene::live::casters::kCollapseTable, "d3_guest_core.h and caster_core.h name the same collapse table");

inline d3::GuestObjects g_cobjs{d3::kMaxCasterPlaneCalls};   // the caster class's ledger (key = CasterObjectId(model, pass)); lane 290: it holds 16 calls an object, the map / rim passes cap theirs at kMaxCalls (CallInputs::max_calls), the plane follows all 16
inline std::atomic<uint64_t> g_ccalls{0}, g_cskipped{0};

// What the sub_82BDED40 hook (draw_origin.h) tells the open caster call: one part was recorded, at this pass site. A part of the open call's own pass counts as keyed, any other site as other.
struct CasterOpen {
  bool active = false;
  uint8_t pass = 0;
  uint32_t keyed = 0, other = 0;
  uint16_t parts[d3::kPlaneMaxParts] = {};   // lane 299: the plane pass's part indices in drawing order (a skipped plane call replays them to the plane tracker)
  uint32_t nparts = 0;
  bool parts_overflow = false;
};
inline thread_local CasterOpen t_copen;
inline d3::PlanePartLedger g_plane_parts;                              // lane 299: (model, call index) -> the parts the last recorded-in-full plane call drew
inline bool (*g_plane_replay)(uint32_t model, uint32_t part) = nullptr;   // lane 299: installed by draw_origin.h (plane::Tracker::OnPartDraw; true = the entry was made)
inline bool (*g_plane_active)() = nullptr;                                // lane 299: installed by draw_origin.h: the plane tracker is registered and the scene records are on; false/null = no plane skip (the call is recorded in full)
inline void NoteCasterPart(uint32_t pass_site, uint32_t part = 0) {
  if (!t_copen.active) return;
  if (d3::CasterPassOfSite(pass_site) == t_copen.pass) {
    ++t_copen.keyed;
    if (t_copen.pass == 2) {   // lane 299
      if (t_copen.nparts < d3::kPlaneMaxParts) t_copen.parts[t_copen.nparts++] = uint16_t(part);
      else t_copen.parts_overflow = true;
    }
  } else ++t_copen.other;
}

class CasterScope {
 public:
  // `part_scope` is the draw_origin.h PartScope slot the marker's tag is built from (the caller passes its address so this header does not depend on draw_origin.h). `recorded` / `tracker_ordinal` = the tracker's
  // result for THIS entry (caster_glue.h PassEntryHook): no record behind a marker means no skip (and no marker).
  template <class PartScope>
  CasterScope(PPCContext& ctx, uint8_t* base, PartScope& t_part, uint8_t pass, bool recorded, uint32_t tracker_ordinal, uint64_t frame) {   // frame = the ONE read of g_guest_frame the entry hook made (tracker and decision agree)
    if (!d3::g_caster_hook_on.load(std::memory_order_acquire)) return;   // pairs with the release stores of the live start-up and the gate's latch
    if (pass < 1 || pass > 3) return;
    if (t_copen.active || t_open.active) {   // a nested call: never decided, never counted
      d3::g_cstats.nested.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t model = ctx.r3.u32;
    model_ = model;   // lane 299
    d3::CallInputs in;
    in.object = d3::CasterObjectId(model, pass), in.frame = frame, in.owner = true, in.caster = true;
    in.max_calls = d3::CasterCallCap(pass);   // lane 290: the plane pass tracks 16 calls an object (the ring's plane proxy makes 12 a frame), map / rim 4
    in.state_readable = recorded, in.state_count = 0;   // the record is the tracker's (written before this decision); an entry it did not record cannot back a marker
    in.now_ms = d3::NowMs();
    in.proof_skew = d3::kHookToStreamOffset;
    in.path = d3::PredictPathCaster([base](uint32_t a, uint32_t& v) {
      if (!wwe2k14::GuestWordReadableFast(a)) return false;
      v = Ld32(base, a);
      return true;
    }, pass, model, ctx.r13.u32);
    d3::g_cstats.calls[pass].fetch_add(1, std::memory_order_relaxed);
    d3::g_cstats.path[pass][uint8_t(in.path)].fetch_add(1, std::memory_order_relaxed);
    if (!recorded) d3::g_cstats.no_record[pass].fetch_add(1, std::memory_order_relaxed);
    const std::shared_ptr<const d3::MarkSet> marks = d3::g_cmarks.Acquire();
    ++g_ccalls;
    call_ = d3::BeginGuestCall(g_cobjs, in, marks.get(), d3::g_cpackets, d3::g_ctally, d3::g_cgate);
    if (call_.out.why == d3::Why::ProofFrame && call_.out.eligible && marks && marks->Has(d3::MarkKey(in.object, call_.out.ordinal))) d3::g_cstats.proof_markable[pass].fetch_add(1, std::memory_order_relaxed);   // lane 268: a MARKED object's call lost to a proof frame (the census's "proof frame" counts every object's)
    if (call_.out.why == d3::Why::Skip &&
        d3::WithdrawIfRecordUnbacked(call_.out, in, recorded ? int64_t(tracker_ordinal) : -1, d3::g_cpackets, d3::g_ctally))   // the record exists under another ordinal: the pass is recorded in full
      d3::g_cstats.ordinal_differs[pass].fetch_add(1, std::memory_order_relaxed);
    if (call_.out.why == d3::Why::Skip) {   // review S2: never (skip OR mark) while an OUTER cache recording is open on this slot: the marker would be sealed into that cache
      const d3::OuterCache oc = d3::CasterOuterRecording([base](uint32_t a, uint32_t& v) {
        if (!wwe2k14::GuestWordReadableFast(a)) return false;
        v = Ld32(base, a);
        return true;
      }, ctx.r13.u32);
      if (d3::WithdrawIfOuterRecording(call_.out, in, oc, d3::g_cpackets, d3::g_ctally)) d3::g_cstats.outer_open[pass].fetch_add(1, std::memory_order_relaxed);
    }
    if (!d3::g_caster_verify) d3::WithdrawIfPassUnskippable(call_.out, in, pass, d3::g_cpackets, d3::g_ctally, d3::g_caster_plane);   // lane 268: --wwe2k14_rdr_caster_d3_plane lets a marked plane call through   // lane 226's finding: the plane's registers are produced inside the pass, never skipped (the verify arm only writes markers)
    if (!d3::g_caster_verify && pass == 2) {   // lane 299 (17.15): the plane skip arm. Refused (the call is recorded in full, which teaches the part list) when no replay is installed or the call has no part list yet; logged once
      if (call_.out.why == d3::Why::Skip) {   // only a real skip decision is looked at: the count below is exactly the refusals of THIS check (Decide's own NotMarkable for unmarked / ineligible calls is not one)
        const bool tracker = g_plane_active && g_plane_replay && g_plane_active();
        const bool have_list = tracker && g_plane_parts.Lookup(model, call_.out.call, plane_list_);   // looked up ONCE: the list the skip is decided on is the list it replays
        if (d3::WithdrawIfPlaneUnreplayable(call_.out, in, pass, have_list, d3::g_cpackets, d3::g_ctally)) {
          d3::g_cstats.plane_no_parts.fetch_add(1, std::memory_order_relaxed);
          static std::atomic<bool> logged{false};
          if (!logged.exchange(true)) REXLOG_INFO("[rdr][d3][casters] plane skip: first refusal ({}): the call is recorded in full (it teaches the part list); counted in 'taken back for want of a part list / tracker'", tracker ? "no part list for this (model, call) yet" : "the plane tracker is not active");
        }
      }
    }
    if (call_.out.why == d3::Why::Skip && d3::g_caster_verify) {   // the verify arm: the marker is written, the original still runs (nothing is dropped); the binder compares the stream's draws with the sequence
      Skip(ctx, base, t_part, pass, call_.out.ordinal, frame, marks ? marks->generation : 0, /*return_one=*/false);
      call_.out.why = d3::Why::SwitchOff;   // from here on an ordinary recorded-in-full call (ended with what it drew)
    } else if (call_.out.why == d3::Why::Skip) {
      skipped_ = true;
      Skip(ctx, base, t_part, pass, call_.out.ordinal, frame, marks ? marks->generation : 0, true);
      if (pass == 2) {   // lane 299: the entries the game's own part draws would have made for the plane tracker (plane_shadow_ours joins the mask draws by them)
        ++d3::g_cstats.plane_skipped;
        d3::g_cstats.plane_replayed_parts.fetch_add(d3::ReplayPlaneParts(plane_list_, [model](uint32_t part) { return g_plane_replay(model, part); }), std::memory_order_relaxed);
      }
      d3::EndGuestCall(g_cobjs, call_, {}, d3::Fault::None, &d3::g_ctally);   // a skipped call is ended too: its entry is what the previous frame's was
      return;
    }
    open_ = true;
    t_copen = {true, pass, 0, 0};
  }
  bool skipped() const { return skipped_; }
  ~CasterScope() {
    if (skipped_ || !open_) return;
    d3::CallInfo info;
    info.keyed = uint16_t(t_copen.keyed), info.other = uint16_t(t_copen.other);
    if (t_copen.pass == 2 && d3::g_caster_plane) g_plane_parts.Store(model_, call_.out.call, t_copen.parts, t_copen.nparts, t_copen.parts_overflow || t_copen.other != 0);   // lane 299: what a later skip of this (model, call) replays
    t_copen = {};
    d3::EndGuestCall(g_cobjs, call_, info, d3::Fault::None, &d3::g_ctally);
  }
  CasterScope(const CasterScope&) = delete;
  CasterScope& operator=(const CasterScope&) = delete;

 private:
  // The marker through the real recorder (as the arena's): one op-15 record tagged {call_site = CasterMarkerCall(pass, ordinal), pass_site = kMarkerPass, object = the model}; the hook's frame and the mark
  // generation travel in the record's +20 / +24 words (base vertex, start index). return_one: the skip returns 1; the verify arm hands the original its own arguments back.
  template <class PartScope>
  void Skip(PPCContext& ctx, uint8_t* base, PartScope& t_part, uint8_t pass, uint32_t ordinal, uint64_t frame, uint32_t generation, bool return_one) {
    ++g_cskipped;
    const uint64_t saved_r1 = ctx.r1.u64, saved_lr = ctx.lr;
    const uint32_t saved_csr = ctx.fpscr.csr;
    const uint32_t model = ctx.r3.u32;
    const uint64_t sv_r3 = ctx.r3.u64, sv_r4 = ctx.r4.u64, sv_r5 = ctx.r5.u64, sv_r6 = ctx.r6.u64, sv_r7 = ctx.r7.u64, sv_r8 = ctx.r8.u64, sv_r9 = ctx.r9.u64, sv_r10 = ctx.r10.u64;
    const double sv_f1 = ctx.f1.f64;
    const uint32_t new_r1 = (uint32_t(saved_r1) - 256u) & ~15u;   // a real guest stack frame for the recorder (it stores below r1): lower r1, 16-aligned, restore after
    const uint32_t record = new_r1 + 64u;                          // 32 bytes the recorder fills and copies
    ctx.r1.u64 = new_r1;
    const PartScope before = t_part;
    PartScope marker{};
    marker.object = model, marker.part = 0, marker.pass_site = d3::kMarkerPass;
    t_part = marker;
    ctx.r3.u64 = record, ctx.r4.u64 = 6, ctx.r5.u64 = uint32_t(frame), ctx.r6.u64 = generation, ctx.r7.u64 = 0;
    ctx.lr = d3::CasterMarkerCall(pass, ordinal) + 4u;   // RecordTag stores lr - 4 as the call site
    sub_82BB10C0(ctx, base);
    t_part = before;
    ctx.r1.u64 = saved_r1;   // only what the caller sees: the stack pointer, the return address, the result and the FP control bits (not the whole context)
    ctx.lr = saved_lr;
    ctx.fpscr.restoreGuestBits(saved_csr);
    if (return_one) {
      ctx.r3.u64 = 1;
    } else {
      ctx.r3.u64 = sv_r3, ctx.r4.u64 = sv_r4, ctx.r5.u64 = sv_r5, ctx.r6.u64 = sv_r6, ctx.r7.u64 = sv_r7, ctx.r8.u64 = sv_r8, ctx.r9.u64 = sv_r9, ctx.r10.u64 = sv_r10;
      ctx.f1.f64 = sv_f1;
    }
  }

  bool skipped_ = false, open_ = false;
  d3::PlanePartList plane_list_;   // lane 299: the part list the plane skip was decided on (one lookup)
  uint32_t model_ = 0;   // lane 299: the open call's model (the plane part ledger's key)
  d3::GuestCall call_;
};

}  // namespace wwe2k14::d3hook
