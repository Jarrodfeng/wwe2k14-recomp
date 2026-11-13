// D3 skip the wrestler's colour call: the guest-side hook (Session 101, lane 245; docs\renderer\SKIP_GAME_WORK_MAP.md 18.9 = 18.7's nine preconditions met). Included by draw_origin.h, which owns the one REX_HOOK_RAW on
// sub_82C04DF0 (the arena's CallScope and this scope both sit in it; the wrestler scope first). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// It mirrors d3_caster_hook.h (lane 228's CasterScope) for the wrestler class: the ledger g_wobjs, the packet table / mark set / gate / tally g_wpackets / g_wmarks / g_wgate / g_wtally (d3_arena.h), the class switch
// g_wrestlers_d3 (CallInputs::wrestler) and the hook switch g_wrestlers_hook_on. NEVER the arena's, the ring's or the casters' globals. A wrestler call is keyed by the model object (like the arena's); its keyed ordinal counts the
// calls of the object that drew only COLOUR-site parts (0x82C0A950, and the mirror copy 0x82C0B94C: NotePart's keyed test), which is the binder's WrestlerChain run count (ScanRuns with wrestler_markers: a wrestler's rim draws
// are ModelPart-class too and are NOT runs of the object, 18.7 finding 4). The decision itself is d3_guest_core.h BeginWrestlerCall (also compiled by tools\d3\d3_unit_test.cpp).
//
// THE SKIP POINT is the arena's: the hook on the function (every entry has the same register state; a skipped call returns 1 = "drew", what the function returns after its body). Only cache path 1 skips (BeginWrestlerCall: the
// path is predicted BEFORE the decision and defaults to Unreadable); paths 2-4 and an outer cache recording get no packet and no marker, in the verify arm too. What a skip does not replay [U, as the arena's]: sub_82BD5460 /
// sub_82BD54D0 and the recorders' own state; the live verify run reads it. On a skip the model-entry readers the mid-asm hook would have run are called here (g_wentry: the light state, the arena worlds, the ring rows AND the wrestler
// palette copy: the wrestler class's bones come from it).
//
// Default OFF: d3::g_wrestlers_hook_on is false unless --wwe2k14_rdr_wrestlers_d3 turned the chain on (live.cpp); the gate (3 resets in 10 s) latches it off for the session (binder.h D3WEndFrame). Off, the cost is one acquire load per
// call. In the VERIFY arm (--wwe2k14_rdr_wrestlers_d3_verify) the marker and packet are written and the original ALWAYS runs: nothing is dropped, the binder compares.
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
#include "d3_arena_hook.h"   // Ld32, g_state_probe (the read-only pre-read of the call's light state)

REX_EXTERN(sub_82BB10C0);   // the HOOKED op-15 recorder (scene_crowd.h): it opens the origin tag from the thread's t_part and the return address

namespace wwe2k14::d3hook {

inline d3::GuestObjects g_wobjs;   // the wrestler class's ledger (key = the model object)
inline std::atomic<uint64_t> g_wcalls{0}, g_wskipped{0};

// fp_probe.h installs the model-entry readers a skipped wrestler call must still run (the mid-asm hook inside the function does not run on a skip): the arena's three (light state, arena worlds, ring rows) AND the wrestler palette.
inline void (*g_wentry)(uint32_t model, uint32_t palette, uint32_t r13, uint64_t frame) = nullptr;
// draw_origin.h (g_d3_wcams_installed, next to the sub_82C04DF0 hook) installs the question 18.7 precondition (7) asks: does THIS hook frame's caster tracker hold a map entry with a palette (the wrestler light camera) and a rim entry with its depth range?
inline void (*g_wcams)(uint64_t frame, bool& have_camera, bool& have_rim) = nullptr;
// ... and (reviewer SF2) g_d3_wspheres_installed: 0 = the object is no sphere skeleton, 1 = lane 242's row of it is in this hook frame, 2 = a known sphere skeleton with no row yet (the skip is refused).
inline int (*g_wspheres)(uint64_t frame, uint32_t object) = nullptr;

// What the sub_82BDED40 hook (draw_origin.h) tells the open wrestler call: one part was recorded, at this pass site.
struct WrestlerOpen {
  bool active = false;
  uint32_t keyed = 0, other = 0;
};
inline thread_local WrestlerOpen t_wopen;
inline void NoteWrestlerPart(uint32_t pass_site) {
  if (!t_wopen.active) return;
  if (pass_site == rdr_adapter::thin::kSiteColour || pass_site == rdr_adapter::thin::kSiteMirror) ++t_wopen.keyed;
  else ++t_wopen.other;
}

class WrestlerScope {
 public:
  // `part_scope` is the draw_origin.h PartScope slot the marker's tag is built from (the caller passes its address so this header does not depend on draw_origin.h).
  template <class PartScope>
  WrestlerScope(PPCContext& ctx, uint8_t* base, PartScope& t_part) {
    if (!d3::g_wrestlers_hook_on.load(std::memory_order_acquire)) return;   // pairs with the release stores of the live start-up and the gate's latch
    if (t_open.active) return;   // an arena / ring call is open on this thread (RingScope opens t_open for a ring call it did not skip): not ours, never decided, never counted (the binder's guard filters the ring's models)
    if (t_wopen.active) {   // a nested call: never decided, never counted
      d3::g_wstats.nested.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    d3::CallInputs in = d3::WrestlerCallInputs();   // path Unreadable until BeginWrestlerCall predicts it
    in.object = ctx.r3.u32, in.frame = frame, in.owner = true, in.state_readable = true, in.state_count = 0;
    in.now_ms = d3::NowMs();
    in.proof_skew = d3::kHookToStreamOffset;   // an object is not skipped in either clock's proof frame
    d3::StateProbe probe;   // the call's light state is read BEFORE the decision: an unreadable state / a full table is Why::Unreadable / StateOverflow, never a skip
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
    const std::shared_ptr<const d3::MarkSet> marks = d3::g_wmarks.Acquire();
    ++g_wcalls;
    call_ = d3::BeginWrestlerCall(
        g_wobjs, in, ctx.r13.u32,
        [base](uint32_t a, uint32_t& v) {
          if (!wwe2k14::GuestWordReadableFast(a)) return false;
          v = Ld32(base, a);
          return true;
        },
        [](uint64_t f, bool& have_camera, bool& have_rim) {
          if (g_wcams) g_wcams(f, have_camera, have_rim);   // no query installed: nothing is known, so the answer stays "no"
        },
        [](uint64_t f, uint32_t object) { return g_wspheres ? g_wspheres(f, object) : 0; },   // no query installed: no object is known to need a sphere row
        marks.get(), d3::g_wpackets, d3::g_wtally, d3::g_wgate, d3::g_wstats);
    if (call_.out.why == d3::Why::Skip && d3::g_wrestlers_verify) {   // the verify arm: the marker is written, the original still runs (nothing is dropped); the binder compares the stream's draws with the sequence
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, /*return_one=*/false);
      call_.out.why = d3::Why::SwitchOff;   // from here on an ordinary recorded-in-full call (ended with what it drew)
    } else if (call_.out.why == d3::Why::Skip) {
      skipped_ = true;
      Skip(ctx, base, t_part, call_.out.ordinal, frame, marks ? marks->generation : 0, true);
      d3::EndGuestCall(g_wobjs, call_, {}, d3::Fault::None, &d3::g_wtally);   // a skipped call is ended too: its entry is what the previous frame's was
      return;
    }
    open_ = true;
    t_wopen = {true, 0, 0};
  }
  bool skipped() const { return skipped_; }
  ~WrestlerScope() {
    if (skipped_ || !open_) return;
    d3::CallInfo info;
    info.keyed = uint16_t(t_wopen.keyed), info.other = uint16_t(t_wopen.other);
    t_wopen = {};
    d3::EndGuestCall(g_wobjs, call_, info, d3::Fault::None, &d3::g_wtally);
  }
  WrestlerScope(const WrestlerScope&) = delete;
  WrestlerScope& operator=(const WrestlerScope&) = delete;

 private:
  // The marker through the real recorder (as the arena's): one op-15 record tagged {call_site = kWrMarkerCall | ordinal, pass_site = kMarkerPass, object = the model}; the hook's frame and the mark generation travel in the
  // record's +20 / +24 words (base vertex, start index). return_one: the skip returns 1 (after the model-entry readers); the verify arm hands the original its own arguments back.
  template <class PartScope>
  void Skip(PPCContext& ctx, uint8_t* base, PartScope& t_part, uint32_t ordinal, uint64_t frame, uint32_t generation, bool return_one) {
    ++g_wskipped;
    const uint64_t saved_r1 = ctx.r1.u64, saved_lr = ctx.lr;
    const uint32_t saved_csr = ctx.fpscr.csr;
    const uint32_t model = ctx.r3.u32, palette = ctx.r5.u32, r13 = ctx.r13.u32;
    if (g_wentry && return_one) g_wentry(model, palette, r13, frame);   // the verify arm runs the original afterwards, so ITS mid-asm hook records the state, the world and the bones; the skip arm must call the readers itself
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
    ctx.lr = d3::kWrMarkerCall + (ordinal & 0xFFu) + 4u;   // RecordTag stores lr - 4 as the call site
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
