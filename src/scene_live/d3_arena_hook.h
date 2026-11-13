// D3 skip the object: the guest-side hook (Session 101, lane 138; docs\renderer\SKIP_GAME_WORK_MAP.md 2.2 / 2.4 / 8.3 item 2 / 10). Included by draw_origin.h, which owns the one REX_HOOK_RAW on sub_82C04DF0
// (a function can have only one hook; this lane's CallScope sits in it, marked `lane 138`). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// THE SKIP POINT (decided by reading the listings, py tools\analysis\list_function.py, 2026-11-12):
//   * sub_82C04DF0 has FOUR static entries: bl from sub_82C13A80 (0x82C13C24), sub_82C13C88 (0x82C13D80), sub_82C13DA8 (0x82C13EA4) and a tail `b` from sub_82C13EC8 (0x82C13ED0), plus the data word
//     0x82210DC8 (a vtable slot). A hook on the caller's bl at 0x82C13D80 would see one of the four and miss the vtable entry; a mid-asm hook cannot return early at all (it only observes). A REX_HOOK on the
//     function itself sees every entry with the same register state (r3 model, r4 pass arg, r5 palette, f1) and can return for the caller. So the skip is a hook ON sub_82C04DF0 (the existing
//     REX_HOOK_RAW in draw_origin.h; the mid-asm hook wwe2k14_model_entry at 0x82C04E1C inside it is unchanged and still runs for every call the game records in full).
//   * sub_82BD7B50 (called at 0x82C04E24) is NOT a cull: it is the model's CACHED COMMAND-LIST manager (lane 138 review B). Four paths (listing 0x82BD7B6C-0x82BD7C44):
//       1 [model+888]==0 or ([model+600]&1)==0 -> returns 0: no cache, the model is drawn fresh;
//       2 [[model+896]+slot*4]!=0 -> local=1, returns 0: a list is in the slot, drawn fresh;
//       3 handle r19=[model+852 or 848] (by [[model+900]+slot*4]) with [r19]==0 -> sub_82BB4AE0 begins recording the model's cache, local=1, returns 0; the function's TAIL (0x82C0C99C, op 82 +
//         sub_82BB4B98) seals it;
//       4 [r19]!=0 -> packs the per-slot constants, op 81 "call cached list" (sub_82C24500), returns 1 (the function then returns 0 at once: the cached list draws the model).
//     The hook skips ONLY on path 1 (lane 237: path 2 = the model's cache is being RECORDED, a marker would be sealed into the persistent cached list; 11.9) (PredictPath reads the same words before deciding); a path-3 call is recorded in full (skipping it would seal an EMPTY cached list and the object would
//     vanish from the game's own stream even with D3 off); a path-4 call is left to the game. On a skip nothing of sub_82BD7B50 or the tail runs: path 1 has no side effect to replay.
//   * NOT replayed (unconfirmed, [U]): sub_82BD5460 / sub_82BD54D0 (a stack local's constructor / destructor, db entries absent) and the recorders' own state (whether a skipped span leaves the next
//     object's records assuming state it never saw). The live check reads it: shadows still right, no new unnamed draws.
//
// Default OFF: d3::g_hook_on is false unless --wwe2k14_rdr_arena_d3 turned the whole chain on AND the record route is built (lane 152: d3::kRecordRouteBuilt = true; owner mode is required, reader.h). The gate latches it off for the session after 3 resets in 10 s (d3_arena.h Gate). Off, the cost is one relaxed load per call.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "../frame_stats.h"  // wwe2k14::g_guest_frame
#include "../guest_read.h"   // GuestWordReadableFast
#include "../renderer_adapter/d3_guest_core.h"

REX_EXTERN(sub_82BB10C0);   // the HOOKED op-15 recorder (scene_crowd.h): it opens the origin tag from the thread's t_part and the return address

namespace wwe2k14::d3hook {

namespace d3 = rdr_adapter::d3;

// fp_probe.h installs the model-entry readers (light state, arena worlds) here: on a skipped call the mid-asm hook inside the function does not run, so the glue calls them itself.
inline void (*g_entry)(uint32_t model, uint32_t palette, uint32_t r13, uint64_t frame) = nullptr;
// lane 152 (D3-F item 16): fp_probe.h installs the READ-ONLY pre-read of the call's light state here (lights::Tracker::ProbeModelCall): readable? room in the frame's tables? Asked BEFORE the decision.
inline bool (*g_state_probe)(uint32_t model, uint32_t r13, uint64_t frame, uint32_t& table_states, uint32_t& ordinal) = nullptr;   // false = unreadable

inline d3::GuestObjects g_objs;
inline std::atomic<uint64_t> g_calls{0}, g_skipped{0};

// What the sub_82BDED40 hook (draw_origin.h) tells the open call: one part was recorded, at this pass site.
struct OpenCall {
  bool active = false;
  uint32_t keyed = 0, other = 0;
};
inline thread_local OpenCall t_open;
inline void NotePart(uint32_t pass_site) {
  if (!t_open.active) return;
  if (pass_site == rdr_adapter::thin::kSiteColour || pass_site == rdr_adapter::thin::kSiteMirror) ++t_open.keyed;
  else ++t_open.other;
}

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

class CallScope {
 public:
  // `part_scope` is the draw_origin.h PartScope slot the marker's tag is built from; the caller passes its address so this header does not depend on draw_origin.h.
  template <class PartScope>
  CallScope(PPCContext& ctx, uint8_t* base, PartScope& t_part) {
    if (!d3::g_hook_on.load(std::memory_order_acquire)) return;   // (N5) pairs with the release stores of the live start-up and the gate's latch
    if (t_open.active) return;   // a nested call: never decided, never counted
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    d3::CallInputs in;
    in.object = ctx.r3.u32, in.frame = frame, in.owner = true, in.state_readable = true, in.state_count = 0;
    in.now_ms = d3::NowMs();   // the gate's clock: the same one the binder reports with
    in.proof_skew = d3::kHookToStreamOffset;   // lane 152 (12): the reader's proof schedule runs one frame after the hook's (J1: +1); an object is not skipped in either frame's proof frame
    d3::StateProbe probe;   // lane 152 (16): the state is read before the decision; an unreadable state / a full table is Why::Unreadable / StateOverflow, never a skip
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
    in.path = d3::PredictPath([base](uint32_t a, uint32_t& v) {
      if (!wwe2k14::GuestWordReadableFast(a)) return false;
      v = Ld32(base, a);
      return true;
    }, in.object, ctx.r13.u32);
    const std::shared_ptr<const d3::MarkSet> marks = d3::g_marks.Acquire();
    ++g_calls;
    call_ = d3::BeginGuestCall(g_objs, in, marks.get(), d3::g_packets, d3::g_tally, d3::g_gate);
    if (call_.out.why == d3::Why::Skip && d3::g_verify) {   // lane 152 (10): the verify arm: the marker is written, the original still runs (nothing is dropped); the binder compares the stream's draws with the sequence
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, /*return_one=*/false);
      call_.out.why = d3::Why::SwitchOff;   // from here on the call is an ordinary recorded-in-full one (ended with what it drew)
    } else if (call_.out.why == d3::Why::Skip) {
      skipped_ = true;
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, true);
      d3::EndGuestCall(g_objs, call_, {}, d3::Fault::None, &d3::g_tally);   // a skipped call is ended too (review fix 2): its entry is what the previous frame's was
      return;
    }
    open_ = true;
    t_open = {true, 0, 0};
  }
  bool skipped() const { return skipped_; }
  ~CallScope() {
    if (skipped_ || !open_) return;
    d3::CallInfo info;
    info.keyed = uint16_t(t_open.keyed), info.other = uint16_t(t_open.other);
    t_open = {};
    d3::EndGuestCall(g_objs, call_, info, d3::Fault::None, &d3::g_tally);
  }
  CallScope(const CallScope&) = delete;
  CallScope& operator=(const CallScope&) = delete;

 private:
  // The skip (path 1 only): the model-entry readers, then the marker through the real recorder, return 1 (what the function returns when it drew).
  template <class PartScope>
  void Skip(PPCContext& ctx, uint8_t* base, PartScope& t_part, uint32_t ordinal, uint64_t frame, uint32_t generation, bool return_one) {
    ++g_skipped;
    const uint64_t saved_r1 = ctx.r1.u64, saved_lr = ctx.lr;
    const uint32_t saved_csr = ctx.fpscr.csr;
    const uint32_t model = ctx.r3.u32, palette = ctx.r5.u32, r13 = ctx.r13.u32;
    // the verify arm runs the original afterwards, so ITS mid-asm hook records the state and the world; the skip path must call the readers itself (the mid-asm hook does not run on a skip)
    if (g_entry && return_one) g_entry(model, palette, r13, frame);
    const uint64_t sv_r3 = ctx.r3.u64, sv_r4 = ctx.r4.u64, sv_r5 = ctx.r5.u64, sv_r6 = ctx.r6.u64, sv_r7 = ctx.r7.u64, sv_r8 = ctx.r8.u64, sv_r9 = ctx.r9.u64, sv_r10 = ctx.r10.u64;   // lane 152: the verify arm hands the original its own arguments back
    const double sv_f1 = ctx.f1.f64;
    // a real guest stack frame for the recorder (it stores below r1): lower r1, 16-aligned, restore after
    const uint32_t new_r1 = (uint32_t(saved_r1) - 256u) & ~15u;
    const uint32_t record = new_r1 + 64u;   // 32 bytes the recorder fills and copies
    ctx.r1.u64 = new_r1;
    // the marker: one op-15 record through the real (hooked) recorder, tagged {call_site = kMarkerCall | ordinal, pass_site = kMarkerPass, object}; the hook's frame and the mark generation travel in the
    // record's +20 / +24 words (base vertex, start index), NOT in the interned key (origin_intern.h never evicts: a frame in the key would exhaust its id space)
    const PartScope before = t_part;
    PartScope marker{};
    marker.object = model, marker.part = 0, marker.pass_site = d3::kMarkerPass;
    t_part = marker;
    ctx.r3.u64 = record, ctx.r4.u64 = 6, ctx.r5.u64 = uint32_t(frame), ctx.r6.u64 = generation, ctx.r7.u64 = 0;
    ctx.lr = d3::kMarkerCall + (ordinal & 0xFFu) + 4u;   // RecordTag stores lr - 4 as the call site
    sub_82BB10C0(ctx, base);
    t_part = before;
    ctx.r1.u64 = saved_r1;   // only what the caller sees: the stack pointer, the return address, the result and the FP control bits (not the whole context)
    ctx.lr = saved_lr;
    ctx.fpscr.restoreGuestBits(saved_csr);
    if (return_one) {
      ctx.r3.u64 = 1;
    } else {   // the verify arm: the original call follows with its own arguments
      ctx.r3.u64 = sv_r3, ctx.r4.u64 = sv_r4, ctx.r5.u64 = sv_r5, ctx.r6.u64 = sv_r6, ctx.r7.u64 = sv_r7, ctx.r8.u64 = sv_r8, ctx.r9.u64 = sv_r9, ctx.r10.u64 = sv_r10;
      ctx.f1.f64 = sv_f1;
    }
  }

  bool skipped_ = false, open_ = false;
  d3::GuestCall call_;
};

}  // namespace wwe2k14::d3hook
