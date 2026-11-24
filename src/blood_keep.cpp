// wwe2k14 - BLD7 (Session 108): the blood-keeping GAME BUG FIX, the glue (GBF-1 in docs/GAME_BUG_FIXES.md; this file is its own unit, src/blood_keep.cpp, EXEB-B1). The rule is src/blood_keep_core.h
// (compiled by tools/game_fixes/blood_keep_test.cpp against a fake game); the plan is docs/research/s108/BLD7_plan.md.
//
// THIS DEPARTS FROM THE ORIGINAL GAME ON PURPOSE (Jarrod's decision 2026-11-22: a legitimate game bug, on by default in every build, no toggle;
// docs/GAME_BUG_FIXES.md). The game forgets a wrestler's blood at its own scene repaints. After each repaint Regrow turns the stored levels back on
// with the game's own functions. Three function-entry hooks (REX_HOOK_RAW, no codegen, no toml), each runs the game's function first:
//   sub_825D99C8  the Swet burst's step: after it, only when its burst branch ran ([Swet+60] != 0 on entry, [Swet+36] != 0 after, 0x825D9ABC / 0x825D9AC8)
//   sub_833693C8  the restore handler: after it, only when its rebuild ran (a CCLM with [+228] != 0, 0x833694E4)
//   sub_825C1888  the scene re-apply's reset: after it (its callers then set only the current level; the items already alive are skipped)
// Compiled unconditionally in both builds (stream-keeping and stream-free): it is game behaviour, not renderer code.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <string>

#include "blood_keep_core.h"
#include "guest_mem.h"   // scenario::U32 / U8
#include "guest_call.h"  // scene_director::Caller

REX_EXTERN(__imp__sub_825D99C8);  // the Swet step (r3 = the Swet object, f1 = the step)
REX_EXTERN(__imp__sub_833693C8);  // the restore handler (r3 = char+1184)
REX_EXTERN(__imp__sub_825C1888);  // the re-apply's reset and clear (r3 = cbdc)
REX_EXTERN(sub_82C31CB0);         // component lookup by tag in a list
REX_EXTERN(sub_827261C0);         // reset a sub-pool of the map's items (map, which)
REX_EXTERN(sub_825C19B0);         // face level set (cbdc, n)
REX_EXTERN(sub_825C1A60);         // transfer part level set (cbdc, n, r5)

namespace wwe2k14::blood_keep {

struct GameEnv {
  scene_director::Caller call;
  GameEnv(PPCContext& c, uint8_t* b) : call(c, b) {}
  static bool Ok(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }
  uint32_t U32(uint32_t a) const { return Ok(a) ? wwe2k14::scenario::U32(a) : 0; }
  uint8_t U8(uint32_t a) const { return Ok(a) ? scenario::U8(a) : 0; }
  uint32_t Call(Fn f, uint64_t r3, uint64_t r4, uint64_t r5) {
    switch (f) {
      case Fn::FindComponent: return call(sub_82C31CB0, {r3, r4});
      case Fn::ResetBlood: return call(sub_827261C0, {r3, r4});
      case Fn::SetFace: return call(sub_825C19B0, {r3, r4});
      case Fn::SetPart: return call(sub_825C1A60, {r3, r4, r5});
    }
    return 0;
  }
  void Log(const std::string& s) { REXLOG_INFO("{}", s); }
};

}  // namespace wwe2k14::blood_keep

REX_HOOK_RAW(sub_825D99C8) {
  using namespace wwe2k14::blood_keep;
  const uint32_t swet = ctx.r3.u32;
  const bool burst = GameEnv::Ok(swet) && wwe2k14::scenario::U32(swet + kSwetBurstOff) != 0;  // read before: the original clears it at its end
  const uint32_t ch = GameEnv::Ok(swet) ? wwe2k14::scenario::U32(swet + kSwetOwnerOff) : 0;
  __imp__sub_825D99C8(ctx, base);
  if (!burst || !GameEnv::Ok(ch) || wwe2k14::scenario::U32(swet + kSwetMapOff) == 0) return;  // the burst branch did not reach its rebuild
  GameEnv e(ctx, base);  // saves the post-original registers; every call restores them
  Regrow(e, "swet_burst", FindCbdc(e, e.U32(ch + kCharListOff)));
}

REX_HOOK_RAW(sub_833693C8) {
  using namespace wwe2k14::blood_keep;
  const uint32_t self = ctx.r3.u32;
  __imp__sub_833693C8(ctx, base);
  const uint32_t list = GameEnv::Ok(self) ? wwe2k14::scenario::U32(self + kRestoreListOff) : 0;
  if (!GameEnv::Ok(list)) return;
  GameEnv e(ctx, base);
  if (!RestoreRebuilt(e, list)) return;  // the original's own branch: no region entries, no rebuild
  Regrow(e, "restore", FindCbdc(e, list));
}

REX_HOOK_RAW(sub_825C1888) {
  using namespace wwe2k14::blood_keep;
  const uint32_t cbdc = ctx.r3.u32;
  __imp__sub_825C1888(ctx, base);
  GameEnv e(ctx, base);
  Regrow(e, "reapply", cbdc);
}
