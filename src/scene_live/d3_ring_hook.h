// D3 skip the call: the guest-side hook of the RING class (Session 101, lane 246; docs\renderer\SKIP_GAME_WORK_MAP.md 16.9). Included by draw_origin.h, which owns the one REX_HOOK_RAW on sub_82C04DF0 (the arena's CallScope sits
// in it too; RingScope is constructed BEFORE it and claims the ring's calls). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// It mirrors d3_arena_hook.h (the arena's CallScope) and d3_caster_hook.h (the casters') with the ring class's own state: the ledger g_robjs, the packet table / mark set / gate / tally g_rpackets / g_rmarks / g_rgate / g_rtally
// (d3_arena.h), the chain switch g_ring_d3 (CallInputs::ring) and the hook switch g_ring_hook_on. It is the five hook-side preconditions of 16.8, built:
//   (1) lane 237's rule applies: the hook sets in.path = PredictPath(...) itself before DecideCall (the colour twin sub_82BD7B50's four paths, the model draw is the same function); only Fresh1 may skip, Fresh2 is Why::CachePath2;
//   (2) the ring tally (g_rtally) carries the path histogram (DecideCall -> NotePath) and the hook counts `fresh2 skipped` / `non-fresh1 skipped` itself (both required 0);
//   (5) a live chain on ring-only globals with its own latch (binder.h: SetD3RingLive, D3REndFrame -> g_ring_hook_on off);
//   (6) ONE call, ONE chain: a call whose model is one of the 13 pointers of the object at [0x836F4C74] (ring_rows_core.h RingTable::ClassOf, read once a frame per thread) belongs to this hook; the arena's CallScope never sees it
//       (it finds t_open set and returns, uncounted), and the binder filters both chains' `calls = runs` guards by the same table (d3_arena.h IsLiveRingModel, published here).
// (3) is the reader's (reader.h TryD3Marker claims IsRingMarkerCall behind g_ring_d3).
//
// THE SKIP POINT is the arena's: the hook on sub_82C04DF0 itself, a skipped call returns r3 = 1. Skipped paths: path 1 only. What a skip does not replay [U, the same as the arena's, d3_arena_hook.h]: sub_82BD5460 / sub_82BD54D0 and
// the recorders' own state. The model-entry readers (light state, arena worlds, ring rows) are called by the D3 glue's g_entry on a skip (fp_probe.h), as for the arena.
//
// The ring-rows record is taken at the ENTRY (ring_rows_glue.h PreRecord, before the decision, as the caster hook's tracker-first order): a skip is withdrawn unless that record exists WITH node rows and under the `call` the packet
// carries (d3::WithdrawIfRingRowsUnbacked). A skip is also refused (and no marker written) while an OUTER cache recording is open on the thread's slot (lane 228 review S2: d3::CasterOuterRecording, [entry+36]).
//
// Default OFF: d3::g_ring_hook_on is false unless --wwe2k14_rdr_ring_d3_live turned the chain on (live.cpp) and the record route is built; the gate (3 resets in 10 s) latches it off for the session. Off, the cost is one relaxed load per
// sub_82C04DF0 call. In the VERIFY arm (--wwe2k14_rdr_ring_d3_live_verify) the marker and packet are written and the original ALWAYS runs: nothing is dropped, the binder compares.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // wwe2k14::g_guest_frame
#include "../guest_read.h"   // GuestWordReadableFast
#include "../renderer_adapter/d3_guest_core.h"
#include "d3_arena_hook.h"   // Ld32, t_open, g_state_probe, g_entry (the arena's open call: the ring's call is open in the same slot, so a nested model call is never decided)
#include "ring_rows_glue.h"  // PreRecord (the ring-rows tracker at the entry) and the ring table reader

REX_EXTERN(sub_82BB10C0);   // the HOOKED op-15 recorder (scene_crowd.h): it opens the origin tag from the thread's t_part and the return address

namespace wwe2k14::d3hook {

static_assert(d3::kRingModelSlots == rdr_scene::live::ringrows::kClassTableCount, "d3_arena.h and ring_rows_core.h name the same class table");

inline d3::GuestObjects g_robjs{d3::kMaxRingCalls};   // the ring class's ledger (key = the model address)
inline std::atomic<uint64_t> g_rcalls{0}, g_rskipped{0};

namespace ringclass {
// The class table as this thread last read it (one read of [0x836F4C74] + 13 pointers a guest frame per thread: a few guest words, not a read per call).
struct Cache {
  uint64_t frame = ~0ull;
  rdr_scene::live::ringrows::RingTable table;
};
inline thread_local Cache t_cache;
template <class Rd>
inline bool IsRingModel(Rd rd, uint64_t frame, uint32_t model) {
  if (t_cache.frame != frame) {
    t_cache.frame = frame;
    d3::g_rstats.table_reads.fetch_add(1, std::memory_order_relaxed);
    if (!rdr_scene::live::ringrows::ReadRingTable(rd, t_cache.table)) d3::g_rstats.table_missing.fetch_add(1, std::memory_order_relaxed);   // no ring in this scene (a menu): the table is invalid and every ClassOf is none
    for (uint32_t i = 0; i < d3::kRingModelSlots; ++i)   // the binder's guards filter by the same pointers (d3::IsLiveRingModel); an invalid table publishes zeros
      d3::g_ring_models[i].store(t_cache.table.valid ? t_cache.table.ptr[i] : 0u, std::memory_order_relaxed);
  }
  return t_cache.table.ClassOf(model) != rdr_scene::kRcNone;
}
}  // namespace ringclass

class RingScope {
 public:
  // `part_scope` is the draw_origin.h PartScope slot the marker's tag is built from (the caller passes its address so this header does not depend on draw_origin.h).
  template <class PartScope>
  RingScope(PPCContext& ctx, uint8_t* base, PartScope& t_part) {
    if (!d3::g_ring_hook_on.load(std::memory_order_acquire)) return;   // pairs with the release stores of the live start-up and the gate's latch
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);   // read once: the class table, the tracker, the decision and the packet agree
    const uint32_t model = ctx.r3.u32;
    const auto rd = [base](uint32_t a, uint32_t& v) {
      if (!wwe2k14::GuestWordReadableFast(a)) return false;
      v = Ld32(base, a);
      return true;
    };
    if (!ringclass::IsRingModel(rd, frame, model)) return;   // not the ring's: the arena's CallScope decides it (precondition 6)
    if (t_open.active) {   // a nested call (inside an open arena / ring call): never decided, never counted
      d3::g_rstats.nested.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    d3::g_rstats.claimed.fetch_add(1, std::memory_order_relaxed);
    // the ring-rows record FIRST (precondition: a skip needs the record behind the marker), at the entry's own registers; the mid-asm hook inside the function will not record this call a second time (ring_rows_glue.h token)
    const rdr_scene::live::ringrows::CallResult rows = wwe2k14::ringrows::PreRecord(model, ctx.r5.u32, frame);
    const bool backed = rows.recorded && rows.n_nodes > 0 && (rows.flags & (rdr_scene::kRrUnreadable | rdr_scene::kRrCountUnknown)) == 0;
    if (!backed) d3::g_rstats.no_rows.fetch_add(1, std::memory_order_relaxed);
    d3::CallInputs in;
    in.object = model, in.frame = frame, in.owner = true, in.ring = true, in.state_readable = true, in.state_count = 0;
    in.now_ms = d3::NowMs();   // the gate's clock: the same one the binder reports with
    in.proof_skew = d3::kHookToStreamOffset;
    d3::StateProbe probe;   // the call's light state is READ before the decision (the packet's `call` is the light-state ref's ordinal); the arena's probe, installed by fp_probe.h
    struct ProbeCtx { uint32_t r13; uint64_t frame; } pctx{ctx.r13.u32, frame};
    if (g_state_probe) {
      probe.ctx = &pctx;
      probe.fn = [](void* c, uint32_t object, bool& readable, uint32_t& count, uint32_t& ordinal) {
        const ProbeCtx& p = *static_cast<ProbeCtx*>(c);
        uint32_t states = 0, ord = 0;
        readable = g_state_probe(object, p.r13, p.frame, states, ord);
        count = states;
        ordinal = ord;
        return readable;
      };
      in.probe = &probe;
    }
    in.path = d3::PredictPath(rd, model, ctx.r13.u32);   // precondition 1 / 2: the hook decides the path itself (the in-struct default Fresh1 is never relied on)
    const std::shared_ptr<const d3::MarkSet> marks = d3::g_rmarks.Acquire();
    ++g_rcalls;
    call_ = d3::BeginGuestCall(g_robjs, in, marks.get(), d3::g_rpackets, d3::g_rtally, d3::g_rgate);
    if (call_.out.why == d3::Why::Skip && backed && call_.out.packet_written && rows.call != call_.out.packet_call) d3::g_rstats.call_differs.fetch_add(1, std::memory_order_relaxed);
    d3::WithdrawIfRingRowsUnbacked(call_.out, in, backed, rows.call, d3::g_rpackets, d3::g_rtally);   // the record exists WITH rows and under the packet's call, or the call is recorded in full
    if (call_.out.why == d3::Why::Skip) {   // lane 228 review S2: never (skip OR mark) while an OUTER cache recording is open on this slot: the marker would be sealed into that cache
      const d3::OuterCache oc = d3::CasterOuterRecording(rd, ctx.r13.u32);
      if (d3::WithdrawIfOuterRecording(call_.out, in, oc, d3::g_rpackets, d3::g_rtally)) d3::g_rstats.outer_open.fetch_add(1, std::memory_order_relaxed);
    }
    if (call_.out.why == d3::Why::Skip && in.path != d3::CachePath::Fresh1) {   // lane 237's bar applied to the ring: cannot happen (Decide files every other path as a drop); counted so the verify run can say 0
      d3::g_rstats.nonfresh1_skipped.fetch_add(1, std::memory_order_relaxed);
      if (in.path == d3::CachePath::Fresh2) d3::g_rstats.fresh2_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    if (call_.out.why == d3::Why::Skip && d3::g_ring_verify) {   // the verify arm: the marker is written, the original still runs (nothing is dropped); the binder compares the stream's draws with the sequence
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, /*return_one=*/false);
      call_.out.why = d3::Why::SwitchOff;   // from here on an ordinary recorded-in-full call (ended with what it drew)
    } else if (call_.out.why == d3::Why::Skip) {
      skipped_ = true;
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, true);
      d3::EndGuestCall(g_robjs, call_, {}, d3::Fault::None, &d3::g_rtally);   // a skipped call is ended too: its entry is what the previous frame's was
      return;
    }
    open_ = true;
    t_open = {true, 0, 0};   // the arena's open-call slot: NotePart counts this call's parts, and the arena's CallScope (constructed next) sees a call in progress and does not touch it
  }
  bool skipped() const { return skipped_; }
  ~RingScope() {
    if (skipped_ || !open_) return;
    d3::CallInfo info;
    info.keyed = uint16_t(t_open.keyed), info.other = uint16_t(t_open.other);
    t_open = {};
    wwe2k14::ringrows::ClearPreRecord();   // a token the mid-asm hook did not consume (it always runs on a full path; belt and braces) must not eat the next call's record
    d3::EndGuestCall(g_robjs, call_, info, d3::Fault::None, &d3::g_rtally);
  }
  RingScope(const RingScope&) = delete;
  RingScope& operator=(const RingScope&) = delete;

 private:
  // The marker through the real recorder (as the arena's): one op-15 record tagged {call_site = kRingMarkerCall | ordinal, pass_site = kMarkerPass, object = the model}; the hook's frame and the mark generation travel in the
  // record's +20 / +24 words (base vertex, start index). return_one: the skip returns 1 (after the model-entry readers); the verify arm hands the original its own arguments back.
  template <class PartScope>
  void Skip(PPCContext& ctx, uint8_t* base, PartScope& t_part, uint32_t ordinal, uint64_t frame, uint32_t generation, bool return_one) {
    ++g_rskipped;
    const uint64_t saved_r1 = ctx.r1.u64, saved_lr = ctx.lr;
    const uint32_t saved_csr = ctx.fpscr.csr;
    const uint32_t model = ctx.r3.u32, palette = ctx.r5.u32, r13 = ctx.r13.u32;
    if (g_entry && return_one) g_entry(model, palette, r13, frame);   // the mid-asm hook inside the function does not run on a skip: the light-state / arena-world / ring-rows readers are called here (the ring-rows one finds its pre-record token and records nothing twice)
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
    ctx.lr = d3::kRingMarkerCall + (ordinal & 0xFFu) + 4u;   // RecordTag stores lr - 4 as the call site
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
  d3::GuestCall call_;
};

}  // namespace wwe2k14::d3hook
