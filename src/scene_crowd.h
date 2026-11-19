// wwe2k14 - the first scene-level pass: the crowd (Session 50 step 3; docs\native\SCENE_MAP.md sections 5 and 7).
//
// The crowd block draw sub_82518340 (db audience_draw_block; listing 0x82518340) records, for each of its 3 groups, the
// group's mesh (op 14 SetStreamSource at 0x8251844C, op 12 SetIndices at 0x8251845C) and then, per member that passes
// its flag and group tests, the member's world matrix as VS constants c76-c78 (op 21, bl 0x82BB27D8 at 0x82518554, r4
// = 76, r5 = the matrix at r1+128, r6 = 3) and one indexed draw of the whole mesh (op 15, bl 0x82BB10C0 at 0x82518574).
// With wwe2k14_scene_crowd 1 the game's code runs as it is, but of each group's members only the first is recorded:
// the others' matrices (what their op-21 records would have carried) go to the command processor's queue
// (CommandProcessor::SceneCrowdPush), which draws them itself at the first member's draw; one closing op-21 record of
// the last member's matrix leaves the library's constant shadow as the original records leave it. 2 = verify: every
// member recorded as always, the matrices queued too; the command processor checks them against the draws.
// Our hooks only change which of the game's records reach the command list; nothing the game computes changes.
// Off (the default): one cvar read per call of the three recorders, then the recorder as before.
//
// Session 87 (wwe2k14_scene_crowd_cards, with wwe2k14_scene_crowd): the crowd's flat cards take the same way. The card
// loop sub_82512F70 (one call per block, from sub_825185B0; listing 0x82512F70) records per visible member the matrix
// as c76-c78 (op 21, bl 0x82BB27D8 at 0x825130AC, r4 = 76, r5 = r1+112, r6 = 3) and one 4-index strip (op 15, bl
// 0x82BB10C0 at 0x825130C4); its frame is 224 bytes (stwu r1,-224 at 0x82512F78), the record scratch r1+80. Run 067
// (coverage trace of 0x82BB10C0's callers): ~14,000 of 20,000 recorded draws in Brock's and Bryan's entrances come
// from 0x825130C8, ~1,400 a frame - most of the entrances' draws. A run is one call (one block).
#pragma once

#include <bit>
#include <rex/system/xmemory.h>  // PhysicalHostOffset (Session 90: never a literal 0x1000)
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/hook.h>

#include "half2_hooks.h"  // wwe2k14::half2::Cp(), Load32
#include "cost_counters.h"  // S101 lane 83
#include "draw_origin.h"  // S101 names: RecordTag in the op-15 recorder hook
#include "renderer_adapter/upd_lean.h"  // S102 U2
#include "scene_live/crowd_skip.h"  // S101 lane 50: P2b / P2c, the game's crowd work dropped while ours draws (switches default off)
#include "scene_live/crowd_hooks.h"  // S101 lane27: the crowd contributor's observation (read only), also hooks sub_82509CB8 / sub_82519508
#include "scene_live/plane_glue.h"  // lane 139: the plane shadow chain's constants (read only)
#include "scene_live/circle_glue.h"  // lane 136: the circle blob shadows' constants (read only), seen by the same game-wide op-21 recorder

REXCVAR_DEFINE_INT32(wwe2k14_scene_crowd, 0, "wwe2k14",
                     "Session 50: the crowd drawn from its members' matrices (SCENE_MAP.md section 7): 0 off; 1 each crowd "
                     "group's first member recorded, the others queued to the command processor, which draws them; 2 "
                     "verify (every member recorded as always and queued; the command processor checks the queue)");
REXCVAR_DEFINE_BOOL(wwe2k14_scene_crowd_cards, false, "wwe2k14",
                    "Session 87 (with wwe2k14_scene_crowd): the crowd's flat cards (sub_82512F70, ~1,400 draws a frame in "
                    "the entrances) queued as the 3D crowd's members are: each block's first card recorded, the others "
                    "drawn by the command processor at that draw");

namespace wwe2k14::scene_crowd {

inline constexpr uint32_t kOp21Return = 0x82518558;   // after bl 0x82BB27D8 at 0x82518554 (listing)
inline constexpr uint32_t kOp15Return = 0x82518578;   // after bl 0x82BB10C0 at 0x82518574 (listing; probe run 040)
inline constexpr uint32_t kGroupReturn = 0x82518450;  // after bl 0x82BB0F30 (the group's op 14) at 0x8251844C
inline constexpr uint32_t kFrameSize = 368;           // stwu r1,-368 at 0x82518348
inline constexpr uint32_t kScratch = 192;             // the record scratch: addi r3,r1,192 (0x82518530)
inline constexpr uint32_t kMatrix = 128;              // the member matrix: addi r5,r1,128 (0x82518524)
// The card loop sub_82512F70 (Session 87; listing 0x82512F70).
inline constexpr uint32_t kCardOp21Return = 0x825130B0;  // after bl 0x82BB27D8 at 0x825130AC
inline constexpr uint32_t kCardOp15Return = 0x825130C8;  // after bl 0x82BB10C0 at 0x825130C4 (trace run 067)
inline constexpr uint32_t kCardFrameSize = 224;          // stwu r1,-224 at 0x82512F78
inline constexpr uint32_t kCardScratch = 80;             // addi r3,r1,80 (0x82513088)
inline constexpr uint32_t kCardMatrix = 112;
inline constexpr uint32_t kFloorFrameSize = 480;           // stwu r1,-480 at 0x8276EAAC (sub_8276EAA0)
inline constexpr uint32_t kFloorScratch = 272;             // addi r3,r1,272 before the op-21 at 0x8276ED38
inline constexpr uint32_t kFloorMatrix = 112;              // addi r5,r1,112 before the op-21 at 0x8276ED38             // addi r5,r1,112 (0x8251307C)

struct Run {
  int mode = 0;
  // The site of the open run: the block draw's or the card loop's return addresses and frame offsets.
  uint32_t op15_return = kOp15Return;
  uint32_t scratch = kScratch;
  uint32_t matrix = kMatrix;
  bool first_valid = false;  // the first member's constants recorded
  bool open = false;         // ... and its draw: the others are queued from here on
  bool cur_valid = false;
  uint32_t first[12] = {};
  uint32_t cur[12] = {};
  std::vector<uint32_t> others;
};
inline thread_local Run t_run;

inline void ReadMatrix(const uint8_t* base, uint32_t address, uint32_t out[12]) {
  for (uint32_t i = 0; i < 12; ++i) out[i] = wwe2k14::half2::Load32(base, address + 4 * i);
}

inline void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t be = std::byteswap(value);
  std::memcpy(base + address + ::rex::memory::detail::PhysicalHostOffset(address), &be, 4);
}

// The run ends (the next group's op 14, or the block's end): the closing record (mode 1) and the queue entry.
// frame_r1 = audience_draw_block's stack frame (r1 inside it).
inline void Flush(PPCContext& ctx, uint8_t* base, uint32_t frame_r1);
inline void ReplayOp21(PPCContext& ctx, uint8_t* base, uint32_t matrix_offset);   // S102 UQ1 review F1 (defined below, after the recorder declarations)

}  // namespace wwe2k14::scene_crowd

REX_EXTERN(__imp__sub_82BB27D8);
REX_EXTERN(__imp__sub_82BB10C0);
REX_EXTERN(__imp__sub_82BB0F30);
REX_EXTERN(__imp__sub_82518340);
REX_EXTERN(__imp__sub_82512F70);

// Review F1: a member / card whose op 21 was skipped is KEPT by the cut after all (the gate latched or tiling came on between the two calls): record its op 21 now, from the matrix still on the loop's stack
// (r1 here is the loop function's own frame: the op-15 recorder is called from inside it; r3 is the scratch record the op-15 call passes; r4 = 76, r6 = 3 are the only calls ever skipped).
inline void wwe2k14::scene_crowd::ReplayOp21(PPCContext& ctx, uint8_t* base, uint32_t matrix_offset) {
  PPCContext saved = ctx;
  ctx.r4.u64 = 76;
  ctx.r5.u64 = saved.r1.u32 + matrix_offset;
  ctx.r6.u64 = 3;
  __imp__sub_82BB27D8(ctx, base);
  ctx = saved;
}

inline void wwe2k14::scene_crowd::Flush(PPCContext& ctx, uint8_t* base, uint32_t frame_r1) {
  Run& r = t_run;
  if (r.open && r.first_valid) {
    const uint32_t count = uint32_t(r.others.size() / 12);
    if (count && r.mode == 1) {
      // The last member's matrix where its record read it, and its op-21 record (the library's shadow c76-c78 then
      // ends as the original records leave it); called inside the block's frame, as the block calls it.
      const uint32_t* last = r.others.data() + 12 * size_t(count - 1);
      for (uint32_t i = 0; i < 12; ++i) Store32(base, frame_r1 + r.matrix + 4 * i, last[i]);
      PPCContext saved = ctx;
      ctx.r1.u64 = frame_r1;
      ctx.r3.u64 = frame_r1 + r.scratch;
      ctx.r4.u64 = 76;
      ctx.r5.u64 = frame_r1 + r.matrix;
      ctx.r6.u64 = 3;
      __imp__sub_82BB27D8(ctx, base);
      ctx = saved;
    }
    // (Review: a run without other members draws nothing more - not pushed; its first could match a later draw.)
    if (rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp(); cp && count) {
      cp->SceneCrowdPush(r.first, r.others.data(), count, r.mode == 2);
    }
  }
  r.mode = 0;
  r.first_valid = r.open = r.cur_valid = false;
  r.others.clear();
}

// The op-21 recorder: the crowd's member constants.
REX_HOOK_RAW(sub_82BB27D8) {
  using namespace wwe2k14::scene_crowd;
  wwe2k14::q2d_tap::OnOp21(ctx, base);   // S103 I1: read only (the 2D layer's constants c0 / c1 / c28 while a T_Rect / movie scope is open: a thread_local flag test otherwise); before every skip below. (S103 I3 review: the thread's register shadow is NOT updated here: OnOp21Regs runs only before each call of the original recorder below)
  // S101 lane27: observe the member (read only), then the hook goes on as before. This recorder is game-wide: the return address is tested first.
  if (wwe2k14::crowd_live::IsCrowdOp21(uint32_t(ctx.lr)) && wwe2k14::crowd_live::Active()) wwe2k14::crowd_live::OnOp21(ctx, base);
  // S102 UQ1 (R22 cut 6; wwe2k14_rdr_record_skip bit 1): the observer above has read the card's world, so ours has everything it will ever read from this op-21; the recorder itself is not called for a far card the card cut
  // drops (its op-15 is dropped with it: card_thin_core.h Decider::SkipCardOp21 / PairOp15). Only the card loop's site; the legacy runs path (r.first_valid) never skips; MUST stay after the observer (lint: tools/crowd/record_skip_check.py).
  if (wwe2k14::card_thin::KnockOut(uint32_t(ctx.lr), true)) return;   // S102 UQ1 TEST ONLY (wwe2k14_rdr_record_skip bit 0x80): after the observer, before everything else
  if (uint32_t(ctx.lr) == wwe2k14::card_thin::kCardOp21Return && !t_run.first_valid &&
      wwe2k14::card_thin::SkipCardOp21(uint32_t(ctx.lr), ctx.r5.u32, [] { rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp(); return cp && cp->scene_crowd_blocked(); },
                                       [base](uint32_t a) { return wwe2k14::crowd_live::Be32(base, a); }, ctx.r4.u32, ctx.r6.u32))
    return;
  if (!t_run.first_valid && wwe2k14::member_thin::SiteOfOp21(uint32_t(ctx.lr)) >= 0 &&   // S102 UQ2 (bits 2 / 4): a stands / floor member the member cut drops: its op 21 goes with its op 15 (same rules as the cards)
      wwe2k14::member_thin::SkipMemberOp21(uint32_t(ctx.lr), wwe2k14::crowd_live::g_floor_observe.load(std::memory_order_relaxed),
                                           [] { rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp(); return cp && cp->scene_crowd_blocked(); },
                                           ctx.r5.u32, [base](uint32_t a) { return wwe2k14::crowd_live::Be32(base, a); }, ctx.r4.u32, ctx.r6.u32))
    return;
  if (wwe2k14::plane::Active()) wwe2k14::plane::OnOp21(ctx);  // lane 139: the plane shadow chain's constants (sub_82BF22C8 / sub_827ECD10); read only, tests the return address first
  if (rdr_scene::live::circles::Tracker::IsOurs(uint32_t(ctx.lr)) && wwe2k14::circles::Active()) wwe2k14::circles::OnOp21(ctx);  // lane 136: the circle blob shadows' constants (T_CircleShadow, sub_82812E90); read only, tests the return address first
  // S102 U2 (R22 cut 5; wwe2k14_rdr_upd_lean bit 8): this recorder is game-wide and only two return addresses (the block draw's and the card loop's) can ever continue past the test below. A call from
  // anywhere else always ends in the original, whatever the cvar, the open run or the command processor say, so it goes there before the cvar read and the Cp() / blocked query it would make first
  // (pure reads). Same behaviour by construction (tools\upd_lean lints that the branch below keeps both sites).
  if (wwe2k14::upd_lean::On(wwe2k14::upd_lean::kCrowdHooks) && uint32_t(ctx.lr) != kOp21Return && uint32_t(ctx.lr) != kCardOp21Return) {
    wwe2k14::q2d_tap::OnOp21Regs(ctx, base);   // S103 I3 review: the register shadow, only where the original recorder runs
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  int mode = REXCVAR_GET(wwe2k14_scene_crowd);
  Run& r = t_run;
  // (Review: with MSAA - predicated tiling - the command processor cannot use the runs: every member recorded.)
  if (mode && !r.first_valid) {
    if (rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp(); !cp || cp->scene_crowd_blocked()) mode = 0;
  }
  // (Session 87: the card loop's site too, with its own switch; an open run continues only at its own site.)
  const uint32_t lr = uint32_t(ctx.lr);
  const bool card = lr == kCardOp21Return && (r.first_valid ? r.op15_return == kCardOp15Return
                                                             : REXCVAR_GET(wwe2k14_scene_crowd_cards));
  const bool block = lr == kOp21Return && (!r.first_valid || r.op15_return == kOp15Return);
  if ((!mode && !r.first_valid) || !(block || card) || ctx.r4.u32 != 76 || ctx.r6.u32 != 3) {
    wwe2k14::q2d_tap::OnOp21Regs(ctx, base);   // S103 I3 review
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  if (!r.first_valid) {
    r.mode = mode;
    r.op15_return = card ? kCardOp15Return : kOp15Return;
    r.scratch = card ? kCardScratch : kScratch;
    r.matrix = card ? kCardMatrix : kMatrix;
    ReadMatrix(base, ctx.r5.u32, r.first);
    r.first_valid = true;
    wwe2k14::q2d_tap::OnOp21Regs(ctx, base);   // S103 I3 review
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  if (!r.open) {  // (a second constant record before the first draw: not the pattern - recorded, the run restarts)
    ReadMatrix(base, ctx.r5.u32, r.first);
    wwe2k14::q2d_tap::OnOp21Regs(ctx, base);   // S103 I3 review
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  ReadMatrix(base, ctx.r5.u32, r.cur);
  r.cur_valid = true;
  if (r.mode == 2) {
    wwe2k14::q2d_tap::OnOp21Regs(ctx, base);   // S103 I3 review: mode 1 never calls the recorder here, so it never reaches the shadow either
    __imp__sub_82BB27D8(ctx, base);
  }
}

// The op-15 recorder: the crowd's member draws.
REX_HOOK_RAW(sub_82BB10C0) {
  if (wwe2k14::card_thin::KnockOut(uint32_t(ctx.lr), false)) return;   // S102 UQ1 TEST ONLY (bit 0x80): the draw record is not made
  wwe2k14::draw_origin::RecordTag origin_tag(ctx, base, 15);  // S101 names: the record carries its submitter (draw_origin.h)
  if (wwe2k14::plane::Active()) wwe2k14::plane::OnBlurDraw(ctx);  // lane 139: the plane blur's draw (LR 0x827EEC88): snapshot of its constants; read only
  using namespace wwe2k14::scene_crowd;
  Run& r = t_run;
  // S101 lane 50 (P2b): a crowd block / card draw ours replaces is not recorded (own switch; only outside an open scene_crowd run).
  if (!r.first_valid) {   // lane 175: DropCard = the far cards thin record (card_thin_hook.h; off unless --wwe2k14_rdr_card_cut)
    rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
    const bool tiled = cp && cp->scene_crowd_blocked();   // S102 T2: MSAA / predicated tiling: member_thin's guard (a tiled frame runs the crowd draw once per tile): every card stays
    if (wwe2k14::crowd_skip::DropDraw(uint32_t(ctx.lr)) || wwe2k14::card_thin::DropCard(uint32_t(ctx.lr), tiled)) return;
    if (wwe2k14::card_thin::TakeReplay()) wwe2k14::scene_crowd::ReplayOp21(ctx, base, wwe2k14::scene_crowd::kCardMatrix);   // S102 UQ1 review F1
  }
  if (!r.first_valid && wwe2k14::member_thin::IsMemberSite(uint32_t(ctx.lr)) && wwe2k14::member_thin::Observing()) {  // lane 272: the stands / floor members' thin record (member_thin_hook.h; inert as built). Tiled = MSAA / predicated tiling (the crowd draw runs once per tile); cp is NULL under guest_gpu_only (default): not tiled there, a doubled tally would be caught by CheckTally (hole, latch).
    rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
    if (wwe2k14::member_thin::DropMember(uint32_t(ctx.lr), cp && cp->scene_crowd_blocked(), wwe2k14::crowd_live::g_floor_observe.load(std::memory_order_relaxed))) return;
    if (wwe2k14::member_thin::TakeReplay())   // S102 UQ2 review F1: the cut kept a member whose op 21 was skipped: record it now
      wwe2k14::scene_crowd::ReplayOp21(ctx, base, uint32_t(ctx.lr) == wwe2k14::member_thin::kFloorOp15Return ? wwe2k14::scene_crowd::kFloorMatrix : wwe2k14::scene_crowd::kMatrix);
  }
  if (!r.first_valid || uint32_t(ctx.lr) != r.op15_return) {
    __imp__sub_82BB10C0(ctx, base);
    return;
  }
  if (!r.open) {
    __imp__sub_82BB10C0(ctx, base);
    r.open = true;
    return;
  }
  if (!r.cur_valid) {  // (a draw without its constants: not the pattern)
    __imp__sub_82BB10C0(ctx, base);
    return;
  }
  r.others.insert(r.others.end(), r.cur, r.cur + 12);
  r.cur_valid = false;
  if (r.mode == 2) __imp__sub_82BB10C0(ctx, base);
}

// The next group's op 14 (SetStreamSource): the previous group's run ends before it.
REX_HOOK_RAW(sub_82BB0F30) {
  using namespace wwe2k14::scene_crowd;
  wwe2k14::member_thin::OnOp14(uint32_t(ctx.lr));  // lane 272: a stands group / floor object mesh begins a member run
  if (t_run.first_valid && t_run.op15_return == kOp15Return && uint32_t(ctx.lr) == kGroupReturn) {
    Flush(ctx, base, ctx.r1.u32);
  }
  __imp__sub_82BB0F30(ctx, base);
}

// S102 UQ1 / UQ2: after a loop that skipped its last op 21, ONE op 21 with that member's world (the constant shadow then ends the loop as the game's own would), as Flush does for the runs.
inline void RecordTailOp21(PPCContext& ctx, uint8_t* base, uint32_t frame_r1, uint32_t scratch, uint32_t matrix, const uint32_t w[12]) {
  for (uint32_t i = 0; i < 12; ++i) wwe2k14::scene_crowd::Store32(base, frame_r1 + matrix + 4 * i, w[i]);
  PPCContext saved = ctx;
  ctx.r1.u64 = frame_r1;
  ctx.r3.u64 = frame_r1 + scratch;
  ctx.r4.u64 = 76;
  ctx.r5.u64 = frame_r1 + matrix;
  ctx.r6.u64 = 3;
  __imp__sub_82BB27D8(ctx, base);
  ctx = saved;
}


// The block's end: the last group's run ends.
REX_HOOK_RAW(sub_82518340) {
  using namespace wwe2k14::scene_crowd;
  const uint32_t entry_r1 = ctx.r1.u32;
  wwe2k14::crowd_skip::t_draws.BeginRun();  // S101 lane 50: the block draw is also a run boundary of the (refused) P2b trigger rule
  wwe2k14::member_thin::OnMemberLoopEntry(wwe2k14::member_thin::kStands);   // S102 UQ2
  wwe2k14::cost::CrowdSpan cost_span;  // S101 lane 83: the crowd block's recording (to the end of the call)
  __imp__sub_82518340(ctx, base);
  {   // S102 UQ2: the last stands member of the call had its op 21 skipped: record ONE op 21 with its world
    uint32_t tail[12];
    if (wwe2k14::member_thin::TakeMemberTail(wwe2k14::member_thin::kStands, tail)) RecordTailOp21(ctx, base, entry_r1 - kFrameSize, kScratch, kMatrix, tail);
  }
  if (t_run.first_valid && t_run.op15_return == kOp15Return) Flush(ctx, base, entry_r1 - kFrameSize);
}

// Session 87: the card loop's end (one block's cards = one run).
REX_HOOK_RAW(sub_82512F70) {
  using namespace wwe2k14::scene_crowd;
  const uint32_t entry_r1 = ctx.r1.u32;
  wwe2k14::bake_skip::OnCardLoop(ctx.r3.u32);  // lane 155: the type's cards are asked for (the bake skip's ledger)
  wwe2k14::card_thin::OnCardLoop();  // lane 175: a type loop begins (the first card draw of it is kept)
  wwe2k14::crowd_live::t_card_block = ctx.r3.u32;  // S101 lane27: the card loop's block i (r3 at entry), for the op-21 observer
  wwe2k14::cost::CrowdSpan cost_span;  // S101 lane 83: the card loop's recording
  __imp__sub_82512F70(ctx, base);
  {   // S102 UQ1 review F1: the loop skipped its last card's op-21: record ONE op-21 with that card's world (the game's constant shadow then ends the loop as it would have), the way Flush does
    uint32_t tail[12];
    if (wwe2k14::card_thin::TakeTail(tail)) RecordTailOp21(ctx, base, entry_r1 - kCardFrameSize, kCardScratch, kCardMatrix, tail);
  }
  if (t_run.first_valid && t_run.op15_return == kCardOp15Return) Flush(ctx, base, entry_r1 - kCardFrameSize);
}

// S102 UQ2 (bit 4): the floor object's draw (sub_8276EAA0; frame 480, scratch r1+272 = addi r3,r1,272 at 0x8276ECF8, matrix r1+112 = addi r5,r1,112 at 0x8276ECD8). A hook only for the loop-exit op 21 of a skipped tail.
REX_EXTERN(__imp__sub_8276EAA0);
REX_HOOK_RAW(sub_8276EAA0) {
  // S103 H: the frame is OURS: the floor-seat group's draw (the seated ringside people, a task beside the crowd entry: scene_skip_cut.py REACH, review F5) is skipped; the floor members' op-21 reader inside it is lane A / D's to move
  if (wwe2k14::scene_skip::Ours(::scene_skip::Hook::FloorSeats)) return;
  const uint32_t entry_r1 = ctx.r1.u32;
  wwe2k14::member_thin::OnMemberLoopEntry(wwe2k14::member_thin::kFloor);
  __imp__sub_8276EAA0(ctx, base);
  uint32_t tail[12];
  if (wwe2k14::member_thin::TakeMemberTail(wwe2k14::member_thin::kFloor, tail)) RecordTailOp21(ctx, base, entry_r1 - wwe2k14::scene_crowd::kFloorFrameSize, wwe2k14::scene_crowd::kFloorScratch, wwe2k14::scene_crowd::kFloorMatrix, tail);
}
