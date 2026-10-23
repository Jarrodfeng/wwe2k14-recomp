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
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/hook.h>

#include "half2_hooks.h"  // wwe2k14::half2::Cp(), Load32

REXCVAR_DEFINE_INT32(wwe2k14_scene_crowd, 0, "wwe2k14",
                     "Session 50: the crowd drawn from its members' matrices (SCENE_MAP.md section 7): 0 off; 1 each crowd "
                     "group's first member recorded, the others queued to the command processor, which draws them; 2 "
                     "verify (every member recorded as always and queued; the command processor checks the queue)");

namespace wwe2k14::scene_crowd {

inline constexpr uint32_t kOp21Return = 0x82518558;   // after bl 0x82BB27D8 at 0x82518554 (listing)
inline constexpr uint32_t kOp15Return = 0x82518578;   // after bl 0x82BB10C0 at 0x82518574 (listing; probe run 040)
inline constexpr uint32_t kGroupReturn = 0x82518450;  // after bl 0x82BB0F30 (the group's op 14) at 0x8251844C
inline constexpr uint32_t kFrameSize = 368;           // stwu r1,-368 at 0x82518348
inline constexpr uint32_t kScratch = 192;             // the record scratch: addi r3,r1,192 (0x82518530)
inline constexpr uint32_t kMatrix = 128;              // the member matrix: addi r5,r1,128 (0x82518524)

struct Run {
  int mode = 0;
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
  std::memcpy(base + address + (address >= 0xE0000000u ? 0x1000u : 0u), &be, 4);
}

// The run ends (the next group's op 14, or the block's end): the closing record (mode 1) and the queue entry.
// frame_r1 = audience_draw_block's stack frame (r1 inside it).
inline void Flush(PPCContext& ctx, uint8_t* base, uint32_t frame_r1);

}  // namespace wwe2k14::scene_crowd

REX_EXTERN(__imp__sub_82BB27D8);
REX_EXTERN(__imp__sub_82BB10C0);
REX_EXTERN(__imp__sub_82BB0F30);
REX_EXTERN(__imp__sub_82518340);

inline void wwe2k14::scene_crowd::Flush(PPCContext& ctx, uint8_t* base, uint32_t frame_r1) {
  Run& r = t_run;
  if (r.open && r.first_valid) {
    const uint32_t count = uint32_t(r.others.size() / 12);
    if (count && r.mode == 1) {
      // The last member's matrix where its record read it, and its op-21 record (the library's shadow c76-c78 then
      // ends as the original records leave it); called inside the block's frame, as the block calls it.
      const uint32_t* last = r.others.data() + 12 * size_t(count - 1);
      for (uint32_t i = 0; i < 12; ++i) Store32(base, frame_r1 + kMatrix + 4 * i, last[i]);
      PPCContext saved = ctx;
      ctx.r1.u64 = frame_r1;
      ctx.r3.u64 = frame_r1 + kScratch;
      ctx.r4.u64 = 76;
      ctx.r5.u64 = frame_r1 + kMatrix;
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
  int mode = REXCVAR_GET(wwe2k14_scene_crowd);
  Run& r = t_run;
  // (Review: with MSAA - predicated tiling - the command processor cannot use the runs: every member recorded.)
  if (mode && !r.first_valid) {
    if (rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp(); !cp || cp->scene_crowd_blocked()) mode = 0;
  }
  if ((!mode && !r.first_valid) || uint32_t(ctx.lr) != kOp21Return || ctx.r4.u32 != 76 || ctx.r6.u32 != 3) {
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  if (!r.first_valid) {
    r.mode = mode;
    ReadMatrix(base, ctx.r5.u32, r.first);
    r.first_valid = true;
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  if (!r.open) {  // (a second constant record before the first draw: not the pattern - recorded, the run restarts)
    ReadMatrix(base, ctx.r5.u32, r.first);
    __imp__sub_82BB27D8(ctx, base);
    return;
  }
  ReadMatrix(base, ctx.r5.u32, r.cur);
  r.cur_valid = true;
  if (r.mode == 2) __imp__sub_82BB27D8(ctx, base);
}

// The op-15 recorder: the crowd's member draws.
REX_HOOK_RAW(sub_82BB10C0) {
  using namespace wwe2k14::scene_crowd;
  Run& r = t_run;
  if (!r.first_valid || uint32_t(ctx.lr) != kOp15Return) {
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
  if (t_run.first_valid && uint32_t(ctx.lr) == kGroupReturn) Flush(ctx, base, ctx.r1.u32);
  __imp__sub_82BB0F30(ctx, base);
}

// The block's end: the last group's run ends.
REX_HOOK_RAW(sub_82518340) {
  using namespace wwe2k14::scene_crowd;
  const uint32_t entry_r1 = ctx.r1.u32;
  __imp__sub_82518340(ctx, base);
  if (t_run.first_valid) Flush(ctx, base, entry_r1 - kFrameSize);
}
